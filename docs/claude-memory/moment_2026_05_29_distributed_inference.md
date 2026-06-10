---
name: moment-2026-05-29-distributed-inference
description: "2026-05-29 — first distributed Transformer inference across two ./p-kernel nodes over the public relay. Phase D core: the Collective layer becomes literal. Also fixed a real stack-overflow crash in the relay path."
metadata: 
  node_type: memory
  type: project
  originSessionId: ea4edf68-51f5-4387-a8b6-bcec32dd0fe8
---

**2026-05-29 — the day two p-kernel nodes computed one neural network together, over the relay.**

Resumed development after Phase C (first UMP APK). Chose the Phase D core: make the existing `dtr` distributed Transformer actually run across nodes via `./relay`, not just on loopback.

## What shipped

The UMP (Linux) `usermain` was missing the *wiring* to drive distributed inference — the infrastructure (K-DDS pub/sub, pmesh routing, relay v2 transport, SWIM→degrade) was all already complete. Added in `arch/linux/{aarch64,x86_64}/usermain.c`:
- `dtr_task()` spawn in `cmd_net()` (needs `drpc_my_node` set first; node id N → drpc_my_node = N-1; even id = TP requester, odd = worker).
- `infer [a b c d]` shell command → `dtr_infer()`.
- `dist` command → `degrade_stat()`.
- `degrade_init()` at boot.

## The proof

Two `./p-kernel` processes + `./relay` on one host. SWIM discovers the peer → degrade flips `FULL → REDUCED (2 nodes)` → `infer 50 20 90 5` on node 1 computes attention head0 locally, ships raw input to node 2 over the relay (`dtr/input` topic), node 2 computes head1 and publishes it back (`dtr/head1`), node 1 concatenates → classifies. **Tensor-parallel Transformer inference across two independently-running kernels over a public UDP relay.** The "Collective" layer of the 5-layer worldview is no longer a metaphor.

## The bug this surfaced — see [[feedback-hosted-relay-stack-overflow]]

The 2-node-over-relay path crashed every time with a garbage PC (SIGBUS/SIGSEGV, pc==addr==random). Root cause was NOT the new dtr wiring (confirmed by bisecting with dtr disabled) — it was four `unsigned char buf[MAX_PKT]` (1416 B each) **stack** buffers in `net_relay.c` blowing the small T-Kernel task stacks (2048–4096) on the deep send/recv call chains. Fix: make them `static` (cooperative scheduler never yields inside them — same pattern as `udp_send`'s `static udp_buf`). Original stack sizes restored; no blanket bump needed.

Also fixed a relay flood: the TP worker re-served K-DDS LATEST_ONLY latched values every poll → ~100 duplicate `head1` publishes per request. Added per-topic `last_req_id` dedup in `dtr_task`.

## Committed — branch `feat/distributed-inference-relay`

Four commits (off `master`; not yet merged/pushed):
1. `feat(dtr)` — distributed inference wiring + the net_relay stack-overflow fix.
2. `fix(swim)` — stop spurious SUSPECT during 3-node bring-up: don't escalate UNKNOWN→SUSPECT, and require SWIM_SUSPECT_ROUNDS(2) consecutive misses for ALIVE→SUSPECT (2-node clusters have no indirect helpers, so one dropped UDP packet must not be a death sentence). Result: zero SELF-SUSPICION/death-throes noise.
3. `feat(dkva)` — wire FULL-mode distributed KV attention; see [[project-dkva-followups]] for what works and what's left.
4. `docs(samples)` — `samples/11_distributed/` runner scripts (2-node REDUCED + 3-node FULL).

Diagnostics (fault handler, `-fstack-protector-all`, env guard) all reverted. aarch64 native + x86_64 cross build clean; 2-node REDUCED demo green.

Cross-links: [[project-ump-android-node]] (Phase D), [[project-dkva-followups]] (FULL-mode follow-ups), [[moment-2026-05-22-cross-arch-kdds]] (same K-DDS layer, loopback), [[moment-2026-05-26-phase-b-substep2]] (relay transport), [[project-pkernel-philosophy]].
