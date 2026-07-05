#!/bin/bash
# ---------------------------------------------------------------------------
# run_unbounded_live.sh — [unbounded-live]: a NOT-YET-WIRED follow-up (honest).
#
# This is a PLACEHOLDER, not a runner. There is NO self-hosted [unbounded-live]
# job yet: the multi-process relay mesh (K real ./p-kernel nodes behind the
# relay, regions forming by RTT, each node's RUNTIME open-topic count asserted
# <= KDDS_TOPIC_MAX while the fleet exceeds R) is DESIGNED but NOT built. PRoot
# has no netns anyway (feedback_proot_sandbox_net_limits,
# feedback_relay_rmem_host_config), so it could not run here even if wired.
#
# What IS proven today: the in-process sweep (run_unbounded.sh) — per-node cost
# is O(R) flat in N on the REAL kernel structs + headers. The over-the-wire row
# and the full unbounded-N cap (ids > 255, after the wire-v2 32-bit id slice)
# are DEFERRED (unbounded_n_design.md §10, U-2/U-3).
#
# This script exits 0 (SKIP, not a pass) so a future self-hosted job can drop in.
# ---------------------------------------------------------------------------
set -u
echo "[unbounded-live] NOT-YET-WIRED follow-up — no self-hosted job exists yet."
echo "  Designed: K real ./p-kernel nodes -> regions form -> assert per-node"
echo "            open-topic count <= KDDS_TOPIC_MAX (flat in fleet) over the wire."
echo "  Blocker here: PRoot has no netns; a multi-process relay mesh cannot run."
echo "  Full ids>255 arm: after wire-v2 (U-2), unbounded_n_design.md §10."
echo "  SKIP (not a failure); this is a placeholder until the [live] job is wired."
exit 0
