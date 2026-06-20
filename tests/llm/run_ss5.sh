#!/bin/bash
# ---------------------------------------------------------------------------
# run_ss5.sh — host cert for SS-5 deterministic expert placement map
#              (arch/common/placement.c, special-structure-mind.md §6/§8.6).
#
# SS-5 makes "which node holds which expert?" a NOCENTRAL LOCAL function:
# place expert e on the node that WINS a rendezvous hash (HRW) of (expert-id,
# alive-member-set). Every node computes the IDENTICAL map from the SAME SWIM
# view — no broadcast, no vote, no leader, no new gossip. placement.c is a
# THIN shim over the EXISTING HRW primitive lookup_responsible()
# (arch/common/lookup.c); it does NOT reimplement sha256. sha256-based =>
# byte-identical ranking across aarch64 / x86_64 / i686.
#
# Certs (all self-contained, IN-PROCESS — the cores are PURE functions of
# (expert_id, members); NO network):
#   [place-deterministic]  one synthetic alive-member set -> the owner of
#                          each expert is IDENTICAL no matter which node
#                          computes it (the fn reads NO self-id). Cross-arch
#                          identity is BY CONSTRUCTION (HRW = sha256 +
#                          byte-pick; lookup.h cross-ABI contract).
#   [place-rehome]         kill the OWNER of one expert -> it re-homes to the
#                          next HRW winner AND every OTHER expert's owner is
#                          UNCHANGED (HRW minimal disruption). FALSIFIABLE: a
#                          non-HRW modulo-N placement reshuffles strictly more
#                          experts when the set shrinks — the cert tells them
#                          apart.
#   [place-balance]        experts spread across a uniform member set
#                          (reported, NOT over-claiming perfect balance).
#   [no-vla] / [nocentral] structural greps (below).
#
# HONESTY (scope): the placement MAP + cert ONLY. Remote-expert EXECUTION
# (firing an expert on its owner node over the mesh) is SS-6 and is DEFERRED.
# Single node => alive set {self} => every expert maps to the one node; the
# map is SS-6's foundation. A true multi-node LIVE placement-convergence run
# is a deferred [live] row — this cert drives the REAL function with
# synthetic member sets.
#
#   ./run_ss5.sh
# Exit 0 = all certs PASS + the nocentral/no-vla greps are clean.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# kernel-tier include chain (placement.c/lookup.c are COMMON_C_SRCS), mirrors
# boot/linux/Makefile INCDIRS for the arch/common + relay + t-kernel headers.
INC="-I$ROOT/arch/linux/aarch64/include \
     -I$ROOT/arch/linux/include \
     -I$ROOT/arch/aarch64/include \
     -I$ROOT/arch/common/include/lp64 \
     -I$ROOT/arch/common/include \
     -I$ROOT/relay \
     -I$ROOT/include/kernel/tkernel \
     -I$ROOT/include"

SRC_PLACE="$ROOT/arch/common/placement.c"
SRC_LOOKUP="$ROOT/arch/common/lookup.c"
SRC_SHA="$ROOT/relay/sha256.c"
SRC_TEST="$HERE/placement_test.c"
SRC_STUB="$HERE/placement_stub.c"

echo "[build] SS-5 placement cert (placement.c + lookup.c HRW reuse) ... (-Werror=vla)"
$CC $CFLAGS $INC \
    "$SRC_TEST" "$SRC_STUB" "$SRC_PLACE" "$SRC_LOOKUP" "$SRC_SHA" \
    -o "$WORK/ss5" \
    || { echo "[build] FAILED"; exit 1; }

# ---- structural [nocentral]: placement.c reuses the HRW primitive and
# introduces NO vote / leader / broadcast / new gossip ----------------------
echo ""
echo "[grep] [nocentral] — placement.c REUSES lookup_responsible (HRW), no vote/leader/broadcast"
# strip comments first so prose ("never a vote, leader, or gossip") can't
# false-positive; then look for actual CALLS to central machinery.
CODE="$(sed -e 's://.*$::' "$SRC_PLACE" | sed -e '/\/\*/,/\*\//d')"
HRW_USE="$(printf '%s\n' "$CODE" | grep -nE 'lookup_responsible[[:space:]]*\(' || true)"
BAD="$(printf '%s\n' "$CODE" | grep -nEi 'raft|quorum|vote|elect|broadcast|leader|gossip_send|drpc_send|kdds_pub' || true)"
if [ -n "$HRW_USE" ] && [ -z "$BAD" ]; then
    echo "  PASS  [nocentral] placement.c calls lookup_responsible() and adds NO"
    echo "        vote/leader/broadcast/gossip — the map is a LOCAL function of"
    echo "        local membership (special-structure-mind.md §6)"
    NC_RC=0
else
    echo "  FAIL  [nocentral]:"
    [ -z "$HRW_USE" ] && echo "        placement.c does NOT call lookup_responsible (HRW reuse missing)"
    [ -n "$BAD" ] && { echo "        forbidden central machinery:"; echo "$BAD"; }
    NC_RC=1
fi

# ---- structural [no-float]: the ranking is integer + sha256 only ----------
echo ""
echo "[grep] [no-float] — no float/double in placement.c (HRW is sha256/integer)"
FLOAT_HITS="$(grep -nE '\b(float|double)\b' "$SRC_PLACE" || true)"
if [ -n "$FLOAT_HITS" ]; then
    echo "  FAIL  [no-float] placement.c uses float/double:"
    echo "$FLOAT_HITS"
    NF_RC=1
else
    echo "  PASS  [no-float] integer + sha256 only (byte-identical across ABIs)"
    NF_RC=0
fi

# ---- [no-vla]: no stack array sized by a runtime dim ----------------------
# placement.c's only arrays are fixed-bound (LOOKUP_KEY_LEN, ST_PLACE_RMAX,
# DNODE_MAX, small test sets) — -Werror=vla above is the compiler backstop;
# this grep is the source-level confirmation.
echo ""
echo "[no-vla] assert no stack array sized by a runtime dim in placement.c"
VLA_HITS="$(grep -nE '\b(float|int|UB|UW|U1|U4|char)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*\[(n|n_members|r|count|expert_id)\]' \
    "$SRC_PLACE" || true)"
if [ -n "$VLA_HITS" ]; then
    echo "  FAIL  [no-vla] a stack array is sized by a runtime dim:"
    echo "$VLA_HITS" | sed 's/^/      /'
    VLA_RC=1
else
    echo "  PASS  [no-vla] no stack array in placement.c sized by a runtime dim"
    echo "        (bounds are LOOKUP_KEY_LEN/ST_PLACE_RMAX/DNODE_MAX; -Werror=vla clean)"
    VLA_RC=0
fi

# ---- run the in-process placement cert ----
echo ""
echo "[run] SS-5 in-process placement cert ..."
"$WORK/ss5"
CERT_RC=$?

echo ""
if [ "$CERT_RC" -eq 0 ] && [ "$NC_RC" -eq 0 ] && [ "$NF_RC" -eq 0 ] && [ "$VLA_RC" -eq 0 ]; then
    echo "[result] PASS"
    echo "[note] remote-expert EXECUTION over the mesh is SS-6 (DEFERRED); a true"
    echo "       multi-node LIVE placement-convergence run is a deferred [live] row."
    echo "       This cert drives the REAL function with synthetic member sets."
    exit 0
else
    echo "[result] FAIL (cert=$CERT_RC nocentral=$NC_RC nofloat=$NF_RC novla=$VLA_RC)"
    exit 1
fi
