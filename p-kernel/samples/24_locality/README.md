# 24_locality — does region locality actually reduce far-traffic? (§4 を数で測る)

`measure_locality.sh` is a **controlled experiment** that puts a number on the
central claim of `docs/architecture/survival-network.md` §4:

> MoE のスパース性 = region 局所ルーティング = 遠方通信を減らす =
> 光速とエネルギーの物理制約への答え

That claim had **never been measured** (俯瞰監査 v3 G25: "エネルギー/性能を一度も
測らない — 全実証が緑だが数字は精度と生存だけ"). This harness produces the missing
numbers, honestly — it reports whether §4 holds **or not**, it does not pass/fail.

## What it does

Same cluster (N=4), same relay, same inference列。唯一変えるのは region 粒度:

| config | `PKERNEL_RTT_ZONE_SIZE` | regions | DKVA `resp` (REGION scope) fans out to |
|--------|-------------------------|---------|----------------------------------------|
| **ON**  | 2 | 2 regions × 2 nodes | in-region peer only (1) |
| **OFF** | 4 | 1 flat region of 4  | everyone (3) — locality disabled |

It drives 5 `dkva infer` from node1 and measures, from **existing observation
points only** (+ one minimal kdds counter, see below):

- **messages / bytes**, split into **near** (same region) vs **far** (cross
  region), via the `kdds` shell command's new `[locality]` line — both as a
  *node1 per-inference-burst delta* and a *cluster cumulative sum* over all 4 nodes.
- **relay DATA bytes/packets** from `relay -v` logs (`type=4` = pmesh/kdds payload).
- **energy proxy** = `near_bytes×1 + far_bytes×K` (K=5; an order-of-magnitude
  stand-in for long-haul cost — **not literal joules**, see the doc).

## Run

```sh
./measure_locality.sh
```

Prints two tables (node1 per-burst, cluster cumulative) and an honest §4 verdict.
Raw logs land in `/tmp/locality.XXXXXX/`. Nodes are ≤4 and torn down by PID kill.

## The one kernel counter we added

`arch/common/kdds.c` gained a cumulative locality counter (`kdds_tx_msgs /
_cross / _bytes / _bytes_cross`), surfaced via `kdds_locality_stats()` and a
`[locality]` line appended to the `kdds` command. It classifies each *delivered*
kdds frame as near/far using `region_is_member()`. This is the only way to get
"how many messages crossed a region per inference"; the relay can't tell (single
localhost relay sees all traffic the same). It is behaviour-preserving — it only
counts, and forces one extra `region_recompute()` per pub for classification.

## Headline finding

Locality **does** reduce total traffic: cluster kdds messages ~**1630 (ON) vs
~4677 (OFF)** and relay DATA bytes ~**0.96 MB vs 2.0 MB** — roughly **2.9× /
2.1× less**. But the *requester's own* outbound is unchanged (its global-scope Q
broadcasts dominate); the win is entirely in region-scoped traffic borne by the
rest of the cluster. And the **latency / light-speed** half of §4 is **not**
measured here — the RTT zone penalty only inflates *reported* RTT for region
formation; it injects no real delay. Full analysis: `docs/benchmarks/locality.md`.
