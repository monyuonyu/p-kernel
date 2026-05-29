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

This demonstrates the FULL/DKVA **mechanism** end-to-end over the relay
(Q broadcast → partial replies → aggregation → "Attention from cluster").
Two honest limitations remain (tracked in `arch/common/dkva.c`):

1. **Fan-in.** All responders share a single `LATEST_ONLY` response slot and
   overwrite each other, so the requester aggregates one peer per round.
   Robust N-way fan-in needs a per-source response topic or a queue QoS.
2. **Empty caches.** A fresh cluster has no prior local inferences, so the
   KV caches — and therefore the partials — are trivial.

## Relay key

The scripts use a fixed 64-hex demo key via `PKERNEL_RELAY_KEY`. For anything
real, generate your own (`openssl rand -hex 32`) and keep it secret; the relay
refuses to start without a key unless you pass `--insecure`.

See also: `docs/phase_b_relay.md` (relay design) and `docs/android.md` (UMP).
