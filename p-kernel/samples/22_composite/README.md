# 22_composite — the composite survival loop (audit G19)

> **G19**: *"閉じた制御ループでなく個別に緑の鎖。一周を連続実行・回帰防御する仕組みが無い。"*
> Every link in the survival chain is green **in isolation**. Nobody had ever
> proven the links hold **at the same time** on **one** cluster. A chain that
> is green link-by-link can still snap when the links are loaded together.

## What this proves

`survive_think_concurrent.sh` boots **one** 4-node cluster over the public
`./relay` and forces three properties to overlap in a single tight window,
then asserts the whole chain held:

| chain link | isolated demo it comes from | proven here, overlapping |
|---|---|---|
| 記憶で考える (memory) | `11_distributed/run_memory_thought.sh` | node4 (never trained, never loaded weights) evals **above chance** using only engrams the swarm gossiped to it via p-fs |
| 同時多発 (concurrent) | `13_survival_loop/concurrent_infer.sh` | node1 **and** node2 issue distributed KV-attention inferences in the **same frame**, repeatedly (per-origin Q, origin-distinct req namespaces) |
| 殺す① (non-origin kill) | `13_survival_loop/kill_one.sh` (A/C) | a non-origin responder (node3) is SIGKILLed **mid-storm**; both origins keep completing every inference, honestly marked `degraded (k/n)`, never E_TMOUT |
| 殺す② (origin kill) | `13_survival_loop/kill_one.sh` (B) | the origin (node1) is SIGKILLed; a survivor (node2) re-issues the **same deterministic question** and completes it |
| 生存数 (world map) | `run_survival_bench.sh` | `world` map converges to the correct live/DEAD split (2 alive / 2 DEAD) |
| 復活 (return + re-fetch) | `run_crash_recovery.sh` / `run_survival_bench.sh` | node3 is revived (fresh, untrained); it re-fetches **both** weights and engrams via p-fs gossip and evals from untrained back to trained-with-memory |
| 無被害 (no crash) | (regression guard) | zero crash / garbage-PC markers across all logs; every survivor process still running |

The crucial difference from the isolated demos: **the kill lands while the
concurrent inferences are in flight AND while a memory-only node is thinking**,
all on the same cluster. See `docs/architecture/composite-scenarios.md` for the
rationale.

## Run

```sh
./survive_think_concurrent.sh
```

Exit code 0 = the chain held under composite load; non-zero = a link snapped.
A markdown result table is printed to **stdout**; the timestamped progress log
goes to **stderr** (so command substitutions never swallow a log line).

Tunables (env): `PORT` (7422), `SETTLE` (8s), `ACC_MIN` (90.0%),
`MEM_MIN` (55.0%), `STORM` (6 frames). Logs: `/tmp/ci22_node{1..4}.log`,
`/tmp/ci22_relay.log`.

### Raw evidence (kill ∧ concurrent ∧ memory, one window)

```
storm begins: node1 + node2 infer in the same frame x6; node4 thinks
node1 <- 'dkva infer 50 20 90 5'
node2 <- 'dkva infer 50 20 90 5'     # 同時多発: same frame, two origins
SIGKILL node3 (pid …) — NON-origin responder, mid-storm
node4 <- 'dtr eval'                  # 記憶で考える: during the storm
```

node1's own log across the kill (note the honest `degraded (3/4)` once node3
dies — never a silent success, never an E_TMOUT):

```
[dkva-cmd] => OK  req=9000001  fp=293        # 4/4, node3 alive
[dkva-cmd] => OK  req=9000002  fp=293
[dkva] degraded (3/4): completed with partial aggregation  req=9000003
[dkva-cmd] => OK  req=9000003  fp=284        # node3 dead — still completes
[dkva] degraded (3/4): completed with partial aggregation  req=9000004
[dkva-cmd] => OK  req=9000004  fp=284
…
```

node4 (never trained, never loaded weights) thinking with the swarm's memory:

```
[ret] engrams loaded from p-fs 'dtr/engrams' (…)
[dtr] eval held-out: acc 26.7% … [ret off]   # weights-only baseline = chance
[dtr] eval held-out: acc 93.3% … [ret ON]    # memory lifts it far above chance
```

## Roles (deliberate)

- **node1** — trainer + concurrent origin #1; later **killed** (origin death).
- **node2** — concurrent origin #2; pulls weights via gossip; **survives**, re-issues after node1 dies, runs the `world` map.
- **node3** — non-origin responder; pulls weights; **killed mid-storm**, then **revived**.
- **node4** — pure **memory-only thinker** (never trains, never loads weights); survives. Its eval is the honest "thinks with the swarm's memory" proof.

## Known flake sources & mitigations

This is the heaviest harness in the tree (real SGD training + p-fs gossip +
SWIM convergence + two kills + a revival, all on one cluster). Flake sources
and how they are handled:

1. **A single stdin line is (rarely) lost on a freshly meshed node.** Every
   gossip-dependent step (`dtr load`, `dtr eval`) is retried with active
   re-sends, exactly as `run_survival_bench.sh` does. `eval_acc` re-sends up to
   4×; `dtr load` up to 20–30×.
2. **SWIM takes time to promote a killed peer stale → DEAD.** The `world` map
   assertion **polls** for up to 90s until the live/DEAD split is correct,
   rather than checking once. Same for the Phase-3 rejoin.
3. **Capturing a log line into an accuracy variable.** All progress output goes
   to **stderr**; only the parsed value reaches stdout inside `$(...)`. (This
   was a real bug during bring-up — see the inline comment on `log()`.)
4. **`pkill -f` has matched the harness's own shell before** (project lore).
   This script only ever kills by explicit PID and reaps on `EXIT`; it never
   uses `pkill -f`.
5. **Region timing.** The composite deliberately runs in **one region**
   (no `PKERNEL_RTT_ZONE_SIZE`) so all four nodes co-aggregate and the
   `degraded (k/n)` denominators are stable.

Observed: **PASS on two consecutive local runs** (aarch64 host, `boot/linux`),
~2.5 min each, no leftover processes. In CI it runs as a **bonus
(continue-on-error) job** until it has proven stable across hosted runners;
promote it to blocking once that holds (see `.github/workflows/ci.yml`).
