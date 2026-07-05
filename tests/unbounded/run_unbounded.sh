#!/bin/bash
# ---------------------------------------------------------------------------
# run_unbounded.sh — host cert for UNBOUNDED-N (unbounded_n_design.md §8/§9).
#
# Thin wrapper around tests/unbounded/Makefile, which builds and runs THREE
# mechanical gates against the REAL shipped kernel headers (no printf-math):
#
#   [unbounded-flat]      measure the REAL per-node kernel structs (sizeof
#                         DKVA_CAGG / DKVA_CAGG_SLOT / NODEMAP / kdds_topics via
#                         dkva.h/kdds.h/nodemap.h) + a runtime R-bound driven
#                         through the REAL nodemap primitive. A byte-budget
#                         _Static_assert makes a [4096] added to the MEASURED
#                         kernel struct fail the build → RED.
#   [unbounded-coupling]  recompile the REAL headers at DNODE_MAX ∈ {64,256,
#                         1024} and PROVE every R-sized quantity (KDDS_TOPIC_MAX,
#                         sizeof kdds_topics, dkva pre-open, sizeof NODEMAP) is
#                         byte-for-byte CONSTANT while the fleet-sized table
#                         (dnode_table) GROWS — the decouple, mechanically.
#   [unbounded-disease]   build the wave-45 CONTROL (pre-fix global-view sizing,
#                         wire bumped): it MUST FAIL to compile (the kdds.h
#                         wave-48 gate + the pmesh beacon MTU). A clean build is
#                         a HARD failure.
#
# Needs no qemu, no training, no relay — exactly what this PRoot sandbox can
# run (feedback_proot_sandbox_net_limits). The REAL multi-process [live] row is
# WIRED for the ThinkPad (run_unbounded_live.sh).
#
#   ./run_unbounded.sh
# Exit 0 = flat PASS + coupling PASS (R-cost constant) + disease REFUSED to build.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

make -C "$HERE" -s check
rc=$?

echo
echo "[live] multi-process relay row — DEFERRED to the ThinkPad self-hosted"
echo "       runner (PRoot here has no netns; feedback_proot_sandbox_net_limits)."
echo "       Wired but NOT run: tests/unbounded/run_unbounded_live.sh"
exit $rc
