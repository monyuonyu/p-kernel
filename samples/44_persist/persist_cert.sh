#!/bin/bash
# ===========================================================================
# 44_persist / persist_cert.sh  —  the ark REMEMBERS across a reboot.
#
# persistence SLICE 1 + SLICE 2 (docs/architecture/30-module/persistence.md). SLICE 0
# made the durable layer honest; these two slices make the mind survive a
# process death on the SAME PKERNEL_PFS_DIR. Every assertion drives a REAL
# production path (pfs_durable_restore / r3_weights_persist /
# r3_weights_restore_or_pretrain) across an ACTUAL kill+restart — no sim,
# no in-process RAM-drop shortcut. Three tags:
#
#   [persist-identity]    SLICE 1. A shell `mind teach` writes the Self-layer
#                         provenance ref self/prov to the durable store. After
#                         kill + reboot on the same dir, pfs_durable_restore +
#                         pfs_dag_restore bring it back with the SAME content-id
#                         (the identity content is byte-identical, not re-derived).
#
#   [persist-mind]        SLICE 2. Teach sky->blue, let the DMN consolidate +
#                         persist rw[], kill, reboot. `mind ask sky` answers
#                         "blue" AND the boot log shows the RESTORE line with
#                         NO "substrate pretrained" line — proving the answer
#                         came from the durable weights, not a re-learn.
#
#   [persist-mind-stale]  SLICE 2 guard (the wave-47 stale-weights trap). A
#                         blob whose header R_NP is corrupted is REFUSED at
#                         boot (one honest "stale ... -> reinitializing" line),
#                         the lazy pretrain rebuilds, and `mind ask sky` is at
#                         CHANCE (NOT "blue", NOT scrambled garbage) — the old
#                         weights are never blindly loaded into the substrate.
#
# Usage:  ./persist_cert.sh        (builds the host target if needed)
# Logs:   /tmp/p44_*.log           Exit non-zero on any failure.
# ===========================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac
KERNEL="$BOOT/p-kernel"
[ -x "$KERNEL" ] || make -C "$BOOT" >/dev/null || { echo "build failed"; exit 1; }

FAIL=0
pass() { echo "$1 PASS"; }
fail() { echo "$1 FAIL: $2"; FAIL=1; }

# run the kernel with a fixed script on stdin, fixed dir; $3.. extra env.
# $1 = logfile  $2 = dir  $3 = script (printf-escaped)  rest = env KEY=VAL
run_kernel() {
    local log="$1" dir="$2" script="$3"; shift 3
    printf "%b" "$script" \
        | timeout 220 env PKERNEL_NODE_ID=1 PKERNEL_PFS_DIR="$dir" "$@" \
              "$KERNEL" >"$log" 2>&1
}

echo "==========================================================="
echo " persistence SLICE 1 + 2 — the ark remembers across reboot"
echo "==========================================================="

# ---------------------------------------------------- [persist-identity]
gate_identity() {
    echo "--- [persist-identity] (SLICE 1): Self lineage survives a reboot ---"
    local D; D=$(mktemp -d /tmp/p44_ident.XXXXXX)
    # Phase 1: a shell teach writes self/prov to the durable store.
    run_kernel /tmp/p44_ident1.log "$D" 'mind teach sky blue\npfs log self/prov\nexit\n'
    local ID1; ID1=$(grep -a "saved 'self/prov'" /tmp/p44_ident1.log \
                     | grep -oE 'content=[0-9a-f]+' | head -1)
    [ -n "$ID1" ] || { fail "[persist-identity]" "self/prov not written in phase 1"; rm -rf "$D"; return; }
    # Phase 2: reboot SAME dir, NO teach — the ref must come back off disk.
    run_kernel /tmp/p44_ident2.log "$D" 'pfs log self/prov\nexit\n'
    grep -aqE 'durable: restored .* block' /tmp/p44_ident2.log \
        || { fail "[persist-identity]" "pfs_durable_restore did not run"; rm -rf "$D"; return; }
    local ID2; ID2=$(grep -aE 'seq=1.*content=' /tmp/p44_ident2.log \
                     | grep -oE 'content=[0-9a-f]+' | head -1)
    [ "$ID1" = "$ID2" ] \
        && pass "[persist-identity]" \
        || fail "[persist-identity]" "content-id changed across reboot ($ID1 vs $ID2)"
    rm -rf "$D"
}

# -------------------------------------------------------- [persist-mind]
gate_mind() {
    echo "--- [persist-mind] (SLICE 2): the learned mind survives a reboot ---"
    local D; D=$(mktemp -d /tmp/p44_mind.XXXXXX)
    # Phase 1: teach sky->blue, wait for DMN consolidation + the rw[] persist.
    run_kernel /tmp/p44_mind1.log "$D" 'mind teach sky blue\nmind wait 120\nmind ask sky\nexit\n'
    grep -aqF '[dmn] sleep: persisted rw[] -> durable store' /tmp/p44_mind1.log \
        || { fail "[persist-mind]" "rw[] was never persisted after consolidation"; rm -rf "$D"; return; }
    [ -f "$D/mind.rw" ] || { fail "[persist-mind]" "mind.rw blob absent"; rm -rf "$D"; return; }
    # Phase 2: reboot SAME dir, ask — must be blue, via RESTORE, no pretrain.
    run_kernel /tmp/p44_mind2.log "$D" 'mind ask sky\nexit\n'
    grep -aqF '[mind] restored learned weights from durable store' /tmp/p44_mind2.log \
        || { fail "[persist-mind]" "restore line absent (did not load the blob)"; rm -rf "$D"; return; }
    if grep -aqF '[mind] substrate pretrained' /tmp/p44_mind2.log; then
        fail "[persist-mind]" "a pretrain ran on reboot — answer would be a re-learn, not a restore"
        rm -rf "$D"; return
    fi
    grep -aqE 'ask "sky" -> "blue"' /tmp/p44_mind2.log \
        && pass "[persist-mind]" \
        || fail "[persist-mind]" "ask sky did not answer blue after restore"
    rm -rf "$D"
}

# -------------------------------------------------- [persist-mind-stale]
gate_mind_stale() {
    echo "--- [persist-mind-stale] (SLICE 2 guard): foreign blob is REFUSED ---"
    local D; D=$(mktemp -d /tmp/p44_stale.XXXXXX)
    run_kernel /tmp/p44_stale1.log "$D" 'mind teach sky blue\nmind wait 120\nexit\n'
    [ -f "$D/mind.rw" ] || { fail "[persist-mind-stale]" "mind.rw not produced to corrupt"; rm -rf "$D"; return; }
    # Corrupt the header R_NP (offset 8, U4 LE) so the dims gate must reject it.
    python3 - "$D/mind.rw" <<'PY' || { echo "python corrupt failed"; }
import sys, struct
p = sys.argv[1]
b = bytearray(open(p,'rb').read())
old = struct.unpack_from('<I', b, 8)[0]
struct.pack_into('<I', b, 8, old ^ 0x1)   # wrong R_NP -> stale header
open(p,'wb').write(b)
PY
    run_kernel /tmp/p44_stale2.log "$D" 'mind ask sky\nexit\n'
    grep -aqF '[mind] stale persisted weights' /tmp/p44_stale2.log \
        || { fail "[persist-mind-stale]" "stale blob was NOT refused (blind-load risk)"; rm -rf "$D"; return; }
    grep -aqF '[mind] substrate pretrained' /tmp/p44_stale2.log \
        || { fail "[persist-mind-stale]" "no lazy pretrain after refusal"; rm -rf "$D"; return; }
    # the answer must NOT be the persisted fact (refusal => fresh substrate, chance).
    if grep -aqE 'ask "sky" -> "blue"' /tmp/p44_stale2.log; then
        fail "[persist-mind-stale]" "answered blue from a refused blob — it WAS blind-loaded"
        rm -rf "$D"; return
    fi
    pass "[persist-mind-stale]"
    rm -rf "$D"
}

gate_identity
gate_mind
gate_mind_stale

echo "-----------------------------------------------------------"
if [ "$FAIL" = 0 ]; then
    echo " RESULT: PASS — identity and the learned mind both survive reboot"
    exit 0
else
    echo " RESULT: FAIL — see the assertions above"
    exit 1
fi
