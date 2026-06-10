---
name: project-dkva-followups
description: "FULL-mode distributed KV attention (DKVA) over the relay. Both 2026-05-29 follow-ups (single-slot fan-in, empty KV caches) FIXED 2026-05-30 via per-source resp topics + dtr_seed_kv_cache warmup; 3-node FULL now aggregates 2 peers with entries=3. Not yet committed."
metadata: 
  node_type: memory
  type: project
  originSessionId: ea4edf68-51f5-4387-a8b6-bcec32dd0fe8
---

DKVA = the FULL-mode path of `dtr_infer` (arch/common/dkva.c): with 3+ live
nodes the degrade controller selects FULL, the even requester broadcasts its
query Q on K-DDS topic `dtr/dkva/q`, every node computes partial attention
over its local KV cache and replies on `dtr/dkva/resp`, and the requester
aggregates + normalises.

## Working as of 2026-05-29 (commit feat(dkva), branch feat/distributed-inference-relay)

- DKVA had **never run end-to-end** before — `DKVA_Q_PKT`(140B)/`DKVA_RESP_PKT`(172B)
  both exceeded `KDDS_DATA_MAX`(128), so `kdds_pub` silently returned E_PAR and
  it always fell back to local MHSA. Raised KDDS_DATA_MAX to 192. (pmesh carries
  up to 1380, so K-DDS payloads up to ~KDDS_DATA_MAX ride fine.)
- ARP is now pre-seeded for the deterministic cluster in `drpc_init` (node d →
  10.1.0.(d+1) / 52:54:00:00:00:0(d+1)). Before, a peer known only via SWIM
  gossip had no ARP entry, so `ip_send` dropped the first datagram (returns -1)
  and one-shot `kdds_pub` never retried — partials were silently lost.
- Responder de-dups by req_id; aggregator de-dups by src_node (LATEST_ONLY
  re-delivers the latched value every poll, which otherwise double-counts).
- 3 nodes form FULL, Q broadcasts over the relay, both peers compute + reply,
  requester takes the "Attention from cluster" path. `samples/11_distributed/
  run_3node_full.sh` exercises it.

## Both follow-ups FIXED 2026-05-30 (branch feat/distributed-inference-relay)

1. **Single-slot fan-in — FIXED via per-source response topics.** Responses now
   go to `dtr/dkva/resp/<node>` (prefix `DKVA_TOPIC_RESP_PFX`), one LATEST_ONLY
   slot per responder, so they no longer overwrite each other. dkva_init opens
   all `DNODE_MAX` resp topics up front (drpc_my_node is still 0xFF at boot, so
   we can't pick "ours" yet); the responder pubs to `h_resp_pub[drpc_my_node]`
   and the requester polls every `h_resp_sub[n]` (n != self), de-duping with a
   `got[n]` flag. Result: "aggregated **2** peers" (was always 1).
2. **Empty KV caches — FIXED via node-distinct warmup.** New public
   `dtr_seed_kv_cache(node)` (dtr.c) runs `run_mhsa_local` on `DTR_KV_SEED_N`(=3)
   node-specific synthetic inputs, which calls `dkva_cache_update`. dkva_task
   calls it once at startup (drpc_my_node is valid by then — the task is
   scheduled after network bring-up). Result: every node reports `entries=3`
   (was 0); aggregated attention is non-trivial and the predicted class shifts.

Verified with `samples/11_distributed/run_3node_full.sh` on aarch64 host (the
demo picks `boot/linux`, NOT `boot/linux_x86_64` — trap: cross-built the wrong
binary first). Both `boot/linux` and `boot/linux_x86_64` rebuild clean; 2-node
REDUCED demo unaffected. Demo script NOTE block updated. **Committed 86c7788**
(dkva.c/dkva.h/dtr.c/dtr.h + run_3node_full.sh, branch feat/distributed-inference-relay).

The 2-node REDUCED tensor-parallel path
([[moment-2026-05-29-distributed-inference]]) remains the other solid
distributed-inference story.
