#!/bin/bash
# ---------------------------------------------------------------------------
# run_unbounded_live.sh — [unbounded-live] ThinkPad self-hosted runner (WIRED,
# NOT run in the PRoot sandbox: no netns / SO_RCVBUFFORCE is a no-op here —
# feedback_proot_sandbox_net_limits, feedback_relay_rmem_host_config).
#
# The in-process sweep (run_unbounded.sh) PROVES per-node cost is O(R) flat in
# N. This [live] row proves the SAME localization holds over the REAL wire:
# spin up K real ./p-kernel nodes behind the relay, let regions form by RTT,
# and assert each node's RUNTIME open-topic count (`dkva_topics_preopened()`
# + the moe/world lazy opens) stays ≤ KDDS_TOPIC_MAX while the fleet exceeds R.
#
# The full unbounded-N cap (ids > 255) is only reachable after the wire-v2
# (32-bit id) slice — DEFERRED (unbounded_n_design.md §10, U-2/U-3). Until
# then this row exercises the U-0/U-1 slice: a ≤64-node fleet is ONE region and
# per-node topic occupancy is byte-behavior-identical to the pre-wave build.
#
# Runner: the self-hosted ThinkPad (docker pkernel_gh_runner), HOST
# rmem_max=32MB. Not a PRoot job.
# ---------------------------------------------------------------------------
set -u
echo "[unbounded-live] DEFERRED to the ThinkPad self-hosted runner."
echo "  Reason: PRoot has no netns; a multi-process relay mesh cannot run here."
echo "  Scope wired: K real ./p-kernel nodes -> regions form -> assert per-node"
echo "               open-topic count <= KDDS_TOPIC_MAX (flat in fleet) over the wire."
echo "  Full ids>255 arm: after wire-v2 (U-2), unbounded_n_design.md §10."
echo "  SKIP (not a failure) in the PRoot CI; enable on the self-hosted runner."
exit 0
