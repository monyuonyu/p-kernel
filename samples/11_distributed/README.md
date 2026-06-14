# 11 — Distributed inference over the relay

Two runnable demos of p-kernel's "Collective" layer: independent `./p-kernel`
processes computing **one** Transformer together, finding each other through
the public `./relay` (the same NAT-traversal substrate the UMP Android fleet
uses).

Both scripts build the kernel and relay if needed, start a relay plus the
peer nodes, drive a couple of inferences on the requester, print a summary,
and clean everything up on exit. They auto-detect the host arch
(`boot/linux` on aarch64, `boot/linux_x86_64` on x86_64).

## `run_2node_reduced.sh` — tensor parallel (REDUCED)

Two nodes. SWIM discovers the peer, the degrade controller drops to
`REDUCED`, and `dtr_infer` splits the multi-head attention across the pair:

```
node 1:  embed + attention head0           ── raw input over relay ──▶  node 2
node 2:                                          embed + attention head1
node 1:  concat(head0, head1) ─▶ FFN ─▶ classifier
```

Expect `TP(REDUCED): class=… ` on node 1 and `TP: head1 …` on node 2. This
path is fully working.

## `run_3node_full.sh` — distributed KV attention (FULL)

Three nodes. With 3 live members degrade selects `FULL`, and `dtr_infer`
broadcasts its query `Q` across the mesh; every node computes partial
attention over its local KV cache and replies, and the requester aggregates.

This runs full FULL/DKVA distributed attention end-to-end over the relay
(Q broadcast → per-source partial replies → aggregation → "Attention from
cluster"). The requester aggregates **all** peers per round (per-source
response topics) and each node seeds its KV cache at startup, so the
partials are non-trivial — see `arch/common/dkva.c`.

## `run_4node_regions.sh` — region-confined attention (regions)

Four nodes split into two latency clusters via a simulated cross-zone RTT
penalty (`PKERNEL_RTT_ZONE_SIZE`). DKVA's topics are region-scoped, so the
region-A requester's query reaches only its same-region peer and the
region-B nodes never participate — distributed attention stays inside the
latency cluster. See `docs/architecture/regions.md`.

## `run_coord_crash.sh` — coordinator crash re-delegates to next min id ([live])

The region coordinator is **not fixed** — it is the region's *minimum live
node-id*, re-derived locally by `region_recompute()` (arch/common/region.c).
This cert proves, with real processes and a real `kill -9`, that the role
**deterministically re-delegates** when the coordinator crashes, and that
distributed aggregation still converges through whoever takes over — no
election, no central single-point-of-failure.

* **Part A** (the invariant itself): a 2-node region `{id0,id1}` viewed from
  the requester `id1`; `kill -9` the coordinator `id0`; after SWIM declares it
  DEAD the requester's `region` print flips `coordinator=node0` →
  `coordinator=node1 (self)` (the next min live id). Disease = a stale
  coordinator; cure = local re-derivation. The node keeps answering (no hang).
* **Part B** (aggregation reconverges): a hub requester folds two remote region
  summaries (rid=2 and rid=3); `kill -9` the `rid=2` coordinator mid-run; the
  requester keeps converging through the survivor (rid=3) and accounts the loss
  honestly as `degraded (k/n)` — no hang, no silent wrong result.

Gate tag: `[coord-crash] PASS`. Scope note: a two-non-hub-node remote region
re-publishing the *same* region's summary after its min dies is blocked by a
separate discovery-layer limitation of the co-located relay harness (non-hub
nodes reliably SWIM-mesh only the hub), not by the re-delegation invariant.

## `run_2node_train_propagate.sh` — R3a: trained weights propagate as memory

Two nodes. Node 1 shows the honest before/after (`dtr eval` at random init
≈ 26.7% held-out, then `dtr train` — 300 epochs of full-batch SGD with
analytic backprop and a real cross-entropy loss, ~0.2 s — then `dtr eval`
again at ~95% train / ~100% held-out) and `dtr save`s the weights as the
versioned p-fs object `dtr/weights`. Node 2 **never trains**: the 2560-byte
weight blob reaches it through P1 chunk replication + P2 ref gossip,
`dtr load` restores it, and `dtr eval` prints the SAME trained accuracy.
When `qemu-x86_64` is available node 2 runs the sibling ABI, proving the
float32 blob is bit-portable across architectures.

## `run_selfc_propagate.sh` — code written on one node runs inside another

Two nodes plus the relay. Node 1 authors C source (`selfc save genome.c`):
the source becomes a content-addressed p-fs object and replicates over the
region like any block. Node 2 runs `selfc run genome.c`: it reads the
source from its own p-fs replica and compiles it **inside its own kernel
process** with the embedded libtcc (`TCC_OUTPUT_MEMORY`), then starts the
result as a new T-Kernel task. No file exchanged, no compiler forked —
see `docs/architecture/self-compile.md` (and its honest list of what is
NOT verified/sandboxed yet). Needs `libtcc-dev` at build time on node 2's
binary; without it the demo still replicates but prints the stub message.

## `run_survival_bench.sh` — survival benchmark (kill K, stay smart)

N `so_node` processes (the same `libpkernel.so` the APK ships) on one v2
relay, measured through three phases: node 1 trains and `dtr save`s the
weights, every node receives them via p-fs gossip and must eval at trained
accuracy (Phase 1); K nodes get SIGKILLed and the survivors must stay
running, converge in SWIM/world, still eval trained, still hold the weight
blob, and still pub/sub (Phase 2); one victim restarts as a fresh untrained
process and must rejoin and re-fetch the weights from the flock (Phase 3).
Prints a markdown result table; exits nonzero if any assertion fails.

```sh
./run_survival_bench.sh        # N=4, K=1
N=8 ./run_survival_bench.sh    # N=8, K=3
```

Measured results: `docs/benchmarks/survival.md`.

## `run_Nnode_scale.sh` — N-node runtime scale test

Parametrized N-node harness (default 16, up to the DNODE_MAX=32 cap) that
asserts full relay registration, membership/world convergence, region
formation, DKVA aggregation, zero resource-exhaustion errors and no crash
signatures. See the header comment for tunables (`ZONE_SIZE`, `SETTLE`).

## Relay key

The scripts use a fixed 64-hex demo key via `PKERNEL_RELAY_KEY`. For anything
real, generate your own (`openssl rand -hex 32`) and keep it secret; the relay
refuses to start without a key unless you pass `--insecure`.

See also: `docs/phase_b_relay.md` (relay design) and `docs/android.md` (UMP).
