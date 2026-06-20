#!/bin/bash
# ---------------------------------------------------------------------------
# run_ss6.sh — host cert for SS-6 cross-node expert firing with local fallback
#              (arch/common/llm/student.c, special-structure-mind.md §5/§8.7).
#
# SS-6 is the F4 capstone: "複数ノードをまたぐ一回の forward" for the byte
# student. The student is MoE; when the SS-1 adaptive router WIDENS K beyond
# K_min on a HARD token, the EXTRA experts (chosen-slot j >= K_min) that SS-5
# placement says live on a PEER node are computed REMOTELY (the peer runs that
# expert's SwiGLU on the [D] f_in vector, ~D floats on the wire) and their [D]
# outputs are summed into moe[] in a FIXED canonical reduction order (ascending
# slot j == the single-node order — CRITIQUE GATE #3). The local K_min experts
# ALWAYS run locally; a remote timeout / absent peer recomputes that expert
# LOCALLY (lose the WIDTH, not correctness — honest degraded).
#
# Certs (all IN-PROCESS, no network — the cert drives the REAL remote-sum code
# path with a STUB peer; the true multi-process forward over the relay is a
# DEFERRED [live] row):
#   [remote-expert-equiv]    a forward that fires the WIDE experts "remotely"
#                            (stub computes the same expert) is BYTE-IDENTICAL
#                            to the pure single-node forward — same logit FNV
#                            hash — across S/M/L; some experts MUST have gone
#                            remote (counted).
#   [remote-expert-fallback] the peer TIMES OUT -> st_forward recomputes that
#                            expert LOCALLY and the result is STILL byte-
#                            identical (fallback loses width, not correctness),
#                            no stall; the honest degraded count is > 0.
#   [remote-falsifiable]     a 1e-6 remote perturbation FAILS the equiv (teeth).
#   [canonical-order]        grep: the MoE reduction sums per-expert [D] outputs
#                            in ASCENDING slot order with NO reassociation.
#   [no-vla]                 grep tripwire over student.c.
#
# HONESTY (scope): the in-process cert drives the REAL canonical-sum + fallback
# code path in st_forward with a stub peer. A true multi-process cross-node
# forward over the relay/mesh is a DEFERRED [live] row (the transport is the
# caller-installed st_set_remote_expert hook; the kernel wires DRPC). The cert
# exercises st_forward (the canonical forward); wiring the hook into the KV
# incremental kv_step generation path is a documented follow-up (run_kv.sh's
# byte-identity is UNCHANGED by SS-6).
#
#   ./run_ss6.sh
# Exit 0 = all certs PASS + the canonical-order / no-vla greps are clean.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c11 -O1 -Wall -Wextra -ffp-contract=off -Werror=vla"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SRC_TEST="$HERE/student_ss6_test.c"
SRC_STU="$ROOT/arch/common/llm/student.c"

echo "[build] SS-6 cert (-O1 -ffp-contract=off -Werror=vla) ..."
$CC $CFLAGS "$SRC_TEST" "$SRC_STU" -o "$WORK/ss6" \
    || { echo "[build] FAILED"; exit 1; }

# ---- [canonical-order]: the MoE reduction sums the per-expert [D] outputs in
# ascending slot order with NO reassociation. We confirm the canonical-sum loop
# is present (`moe[i] += wj * eo[i]` over j ascending) and that NO experts are
# accumulated in a reversed / sorted-by-id order. ---------------------------
echo ""
echo "[grep] [canonical-order] — ascending-slot reduction, no reassociation"
CANON="$(grep -nE 'moe\[i\][[:space:]]*\+=[[:space:]]*wj[[:space:]]*\*[[:space:]]*eo\[i\]' "$SRC_STU" || true)"
# a reversed loop over the chosen experts would be the salty-bug-class regression
REV="$(grep -nE 'for[[:space:]]*\([[:space:]]*int[[:space:]]+j[[:space:]]*=[[:space:]]*nk[[:space:]]*-[[:space:]]*1' "$SRC_STU" || true)"
if [ -n "$CANON" ] && [ -z "$REV" ]; then
    echo "  PASS  [canonical-order] moe[] reduces per-expert eo[] over ascending j,"
    echo "        no reversed expert loop (matches the single-node order bit-for-bit)"
    CANON_RC=0
else
    echo "  FAIL  [canonical-order]:"
    [ -z "$CANON" ] && echo "        canonical sum 'moe[i] += wj * eo[i]' not found"
    [ -n "$REV" ]   && { echo "        a reversed expert loop appeared:"; echo "$REV"; }
    CANON_RC=1
fi

# ---- [no-vla]: grep tripwire over student.c (same as run_kv.sh) ----
echo ""
echo "[no-vla] assert no stack array sized by a runtime dim in student.c"
VLA_HITS="$(grep -nE '\b(float|int|uint8_t|double|char)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*\[(D|E|L|DFF|n|m->d|m->dff|m->nlayer|m->nexpert)\]' \
    "$SRC_STU" || true)"
if [ -n "$VLA_HITS" ]; then
    echo "  FAIL  [no-vla] a stack array is sized by a runtime dim:"
    echo "$VLA_HITS" | sed 's/^/      /'
    VLA_RC=1
else
    echo "  PASS  [no-vla] no stack array in student.c is sized by D/E/L/DFF/n/m->*"
    echo "        (all scratch bound to ST_*_MAX / KMAX / V / ST_MAXSEQ; eo_all is"
    echo "        a file-static fixed [KMAX][DMAX] — not a stack array)"
    VLA_RC=0
fi

# ---- run the equiv + fallback + falsifiability cert ----
echo ""
echo "[run] SS-6 in-process cert (stub peer drives the REAL remote-sum path) ..."
"$WORK/ss6"
CERT_RC=$?

echo ""
echo "[machine] per-tier logit-hash match (single-node vs remote / fallback):"
"$WORK/ss6" --machine | sed 's/^/    /'

echo ""
if [ "$CERT_RC" -eq 0 ] && [ "$CANON_RC" -eq 0 ] && [ "$VLA_RC" -eq 0 ]; then
    echo "[result] PASS"
    echo "[note] HONEST: the in-process cert drives the REAL canonical-sum + fallback"
    echo "       code path with a STUB peer. A true multi-process cross-node forward"
    echo "       over the relay/mesh is a DEFERRED [live] row (transport = the caller-"
    echo "       installed st_set_remote_expert hook; the kernel wires DRPC). The KV"
    echo "       incremental generation path is untouched (run_kv.sh stays identical)."
    exit 0
else
    echo "[result] FAIL (cert=$CERT_RC canonical=$CANON_RC novla=$VLA_RC)"
    exit 1
fi
