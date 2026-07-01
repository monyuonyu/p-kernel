# Composite scenarios — closing the survival loop (audit G19)

## Why composite verification is needed

The structural-gap audit (2nd ed.) calls this **G19**:

> *"閉じた制御ループでなく個別に緑の鎖。一周を連続実行・回帰防御する仕組みが無い。"*
> The survival story is a chain whose links are each green **in isolation**,
> but no single test runs the whole loop and guards it against regression.

By wave 11 the individual demos were all green:

- `run_memory_thought.sh` — the forward pass reads engrams out of p-fs
  (**記憶で考える**).
- `kill_one.sh` / `concurrent_infer.sh` — nodes die mid-inference and the
  swarm completes anyway, while many inferences run at once
  (**死の貫通 + 同時多発**).
- `breathe.sh` — expert specialization (**呼吸**).
- `run_crash_recovery.sh` — a task faults, the node lives, the brain comes back
  from p-fs (**障害復旧**).
- `sprout.sh` — a fresh individual germinates (**発芽**).
- `run_survival_bench.sh` — kill K of N; survivors stay alive *and* smart;
  one returns and inherits the flock's memory (**復活**).

The gap: **a green link is not a green chain.** Each demo isolates its property
on a fresh cluster, so it cannot observe an interaction that only appears when
the properties are loaded *simultaneously*:

- Does the concurrent-inference latch (per-origin Q) still hold when a responder
  dies **between** two same-frame requests?
- Does memory retrieval (`ret_avail()` reading p-fs) still resolve while the
  same node is fanning out / aggregating a distributed inference and a peer is
  being declared DEAD?
- Does a revived node re-fetch **both** weights and engrams, or does one gossip
  channel starve the other under load?

None of those are answered by running the demos one after another. They are
answered only by a harness that makes them **overlap on one cluster** — and by
running that harness **in CI**, so the closed loop is defended against
regression (the second half of G19).

## Scenario definition

`samples/22_composite/survive_think_concurrent.sh` — one 4-node cluster over
the public `./relay`, four phases:

1. **Seed memory.** node1 trains (real SGD), `dtr save`s weights and
   `dtr remember`s engrams to p-fs. node2/node3 pull the weights via gossip;
   node4 is left a pure memory-only thinker.
2. **The storm (the overlap).** node1 **and** node2 issue distributed
   KV-attention inferences in the **same frame**, repeatedly. Mid-storm:
   node3 (non-origin) is SIGKILLed; node4 runs `dtr eval` and must beat chance
   using only gossiped engrams. Then the origin (node1) is SIGKILLed and a
   survivor re-issues the same question. Survivors must stay smart
   (trained-with-memory eval) and the `world` map must show the right live/DEAD
   split.
3. **Return.** node3 is revived (fresh, untrained), rejoins, and re-fetches
   weights **and** engrams via p-fs gossip, going from chance back to
   trained-with-memory accuracy.
4. **No collateral damage.** Zero crash / garbage-PC markers across all logs;
   every survivor process still running.

Assertions: both origins complete **every** same-frame inference (no E_TMOUT);
node3's death is reported as `degraded (k/n)`, never a silent success; node4's
`[ret ON]` held-out accuracy ≥ `MEM_MIN` with "engrams loaded from p-fs"
present; the survivor's `[ret ON]` eval ≥ `ACC_MIN`; the `world` map converges
to 2 alive / 2 DEAD; the revived node climbs from chance to ≥ `ACC_MIN`; zero
crash markers. A markdown table summarizes pass/fail per link.

## Known instability factors

The composite is the heaviest harness in the tree, so the design hardens it
against the known flake sources (see the sample README for the operational
detail):

1. **Lost stdin lines on a freshly meshed node** → every gossip-dependent step
   (`dtr load`, `dtr eval`) actively re-sends and retries, à la
   `run_survival_bench.sh`.
2. **SWIM convergence latency** (stale → DEAD promotion) → the `world` map and
   rejoin checks **poll** with a deadline (≤ 90s) instead of a single read.
3. **stdout pollution** of a parsed accuracy value → all progress log goes to
   **stderr**; only the value reaches `$(...)`.
4. **`pkill -f` matching the harness's own shell** (project lore) → kills are by
   explicit PID only, reaped on `EXIT`.
5. **Region/zone timing** → the composite runs in a single region (no RTT
   zoning) so the `degraded (k/n)` denominators stay stable.

## CI posture

The composite runs as a **bonus (`continue-on-error`) job** (`composite-loop`
in `.github/workflows/ci.yml`), separate from the five existing blocking jobs.
Rationale: it is timing-heavy (SWIM convergence + p-fs gossip + a revival), and
hosted runners are slower and noisier than the dev host; a transient timing
flake should make the closed loop **visible** without blocking unrelated PRs.
It passed on two consecutive local runs (aarch64, `boot/linux`); once it proves
stable across hosted runs it should be promoted to a blocking job — that
promotion is the final nail in G19.
