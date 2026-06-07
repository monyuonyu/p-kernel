# 21_honest_degraded — degraded(k/n) must not lie while gossip is unconverged (G12)

`degraded (k/n)` is p-kernel's honesty contract: when a distributed
KV-attention inference completes with a missing contribution, it says so out
loud instead of silently passing the partial result off as a full success
(death-piercing §3 / survival-network §10).

Wave 10 (audit G2/G8) extended that contract to **other regions**: a whole
remote region that fails to answer is counted into the denominator instead of
vanishing. But it built the remote expectation set from **world-gossip**
`region_id`s — and that quietly made the honesty *number itself* depend on
gossip freshness (audit-2 **G12** 🔴, the most painful side effect):

> The requester groups remote nodes into regions by reading each peer's
> advertised `region_id` from the world map. When a coordinator's region beacon
> has **not yet arrived**, the old code treated *each* remote node as its own
> region (a "conservative" over-count). For a remote region with **several
> members**, that means: before its coordinator beacon converges, every member
> is tallied as a separate expected region (denominator inflated), yet only the
> single coordinator emits a region summary (`rsum`). So `rc_got < rc_cnt0` and
> the requester prints a **false `degraded (2/3)`** — even though every
> contribution was actually folded in. The number lied, and the lie was a
> function of gossip timing.

`concurrent_infer.sh` hides this by sleeping 4 s for beacons to converge before
it measures (and its remote region is a single node, so it never trips the hole
at all). That 4-second convergence wait is itself the evidence that the honesty
number was gossip-timing dependent.

## What this demo does

Topology (`PKERNEL_RTT_ZONE_SIZE=2` over 3 nodes; `zone = internal_id / 2`):

```
  node1 (id0) ─┐
               ├─ zone0  = a MULTI-MEMBER remote region (coordinator = node1)
  node2 (id1) ─┘
  node3 (id2) ─── zone1  = the requester, alone
```

`node3` issues `dkva infer`. Its only remote contribution is **one** `rsum`
from node1, which already folds node2's partial — so the **true** remote-region
count seen by node3 is **1**.

World-gossip is held **unconverged on purpose**:
`PKERNEL_WORLD_BEACON_HOLD_MS` suppresses every node's world self-beacon for the
whole test window, so node3 can never read node1/node2's `region_id` from
gossip. We deliberately **do not** sleep to let world beacons converge (that is
exactly what masks G12). We *do* let SWIM RTT settle so regions form — SWIM is
local (ping RTT), not the gossip whose freshness the honesty number must not
depend on. That is the whole point.

### scenario CONVERGED (regression guard)
Same topology, gossip allowed to converge. Asserts a **healthy** run:
completes, **no** `degraded`, **no** `uncertain`, and records the baseline
fingerprint. The fix must not change the converged path.

### scenario UNCONVERGED (the G12 hole)
Infer from node3 while gossip is held. Asserts:

- **(a) no over-count** — the inflated `(2/3)` / `(1/3)` the old code printed is
  **absent**. The denominator is built from *confirmed-fresh* gossip only.
- **(b) uncertainty is explicit** — a `... uncertain ...`, `provisional` line
  appears (§10). The result never poses as a clean confirmed success while a
  remote node is unconfirmed by gossip.
- **(c) correctness** — completes `=> OK` and its fingerprint **equals the
  converged baseline**, proving every contribution was folded. So a bare
  "degraded (missing contribution)" would have been a *lie*; the honest report
  says `uncertain`, not `missing`.

## How G12 is closed (in `arch/common/dkva.c` + `world.c`)

- New accessor `world_peer_region_fresh(node)` returns a peer's region **only**
  when its beacon is fresh (`age <= WORLD_STALE_MS`); unknown / stale → `-1`.
- The requester builds the confirmed denominator `rc_cnt0` from
  `world_peer_region_fresh()` only. Remote alive nodes whose region cannot be
  confirmed are **not** decreed to be separate regions — they are tracked in a
  separate `uncertain` axis.
- When an `rsum` actually arrives, its sender is confirmed to be a real region
  coordinator, so it is **promoted** from uncertain into the confirmed
  denominator (arrival = proof) — counted correctly in both `k` and `n`.
- While any uncertain alive remote remains, the collection window is **not**
  short-circuited (its `rsum` might still arrive; cutting early would paper over
  "unconverged" with a confident number).
- Reporting (§10): `degraded (k/n; m uncertain): ... (gossip unconverged —
  count provisional)`; and even when `k == n` but `m > 0`, an explicit
  provisional line is printed instead of a silent clean success.

## Run

```sh
./honest_degraded.sh
```

Exit code is non-zero if any assertion fails. Logs: `/tmp/hd21_*.log`. Picks
`boot/linux` (aarch64) or `boot/linux_x86_64` automatically.

Representative honest line from node3 under held gossip:

```
[dkva] degraded (2/2; 1 uncertain): confirmed contributions complete, but 1
remote node(s) unconfirmed by gossip — degraded count provisional until gossip
converges  req=9020001
```

`(2/2)` not `(2/3)`: the denominator is the truth (self + node1's region),
node2 is honestly flagged uncertain rather than over-counted as a phantom
missing region.
