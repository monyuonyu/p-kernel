#!/bin/bash
# ===========================================================================
# 26_ark_backend / run.sh  —  the white-pearl integration (wave 13).
#
# ARK becomes p-fs's durable backend, so the local log-structured filesystem
# (arch/common/arkfs.c) and the distributed content store (p-fs P0,
# arch/common/pfs_block.c) are ONE thing. Selected at runtime with
# PKERNEL_PFS_BACKEND=ark; the ARK image is $PKERNEL_ARK_IMG.
#
# Wiring (arch/linux/pfs_ark.c, reached inline from pfs_block.c):
#   put : a NEW p-fs block -> ark_block_put() + ark_checkpoint()  (fsync'd)
#   get : a P0 (in-memory) MISS falls through to ark_block_get(), whose bytes
#         pfs_block.c re-hashes against the requested id before serving.
#
# Three acts, each asserts; any failure exits non-zero.
#   ACT 1  put X through the kernel shell -> X is in the ARK log ON DISK ->
#          kill -9 (power loss, NO graceful flush) -> remount the SAME image
#          in a fresh process -> `pfs get X` is served back FROM THE ARK LOG
#          (P0 empty after restart), sha256-verified.
#   ACT 2  flip one byte inside a committed block's payload on disk -> remount
#          -> the block's self-verify (crc + sha256) REJECTS it: `pfs get`
#          returns NOT FOUND. The store still mounts; only the rotted block
#          is withheld.
#   ACT 3  crash-safety of the put path: kill -9 the writer the instant after
#          a put, restart -> the committed block survives AND a fresh put on
#          the same image still works (the log is not wedged).
#
# Usage:  ./run.sh            (builds the host target if needed)
# Logs:   /tmp/ark_be_*.log
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

WORK="$(mktemp -d /tmp/ark_be_work.XXXXXX)"
KPIDS=()
cleanup() {
    [ "${#KPIDS[@]}" -gt 0 ] && kill -9 "${KPIDS[@]}" 2>/dev/null
    pkill -9 -P $$ 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

FAIL=0
ok()  { echo "  [PASS] $*"; }
bad() { echo "  [FAIL] $*"; FAIL=1; }

# Launch a SOLO node on ARK image $1, logging to $2, fed the remaining args as
# shell commands (one per ~1.5s), then held alive by a long tail-sleep so the
# caller can kill -9 it mid-flight (a real power loss leaves no chance to
# flush — durability must come from the per-put fsync'd checkpoint, not a
# graceful shutdown). Echoes the kernel PID.
launch() {
    local img="$1" log="$2"; shift 2
    local -a cmds=("$@")
    { sleep 2.5
      for c in "${cmds[@]}"; do printf '%s\n' "$c"; sleep 1.5; done
      sleep 600
    } | env PKERNEL_PFS_BACKEND=ark PKERNEL_ARK_IMG="$img" "$KERNEL" >"$log" 2>&1 &
    echo $!
}

# Poll $1=log for regex $2 up to $3 seconds. 0 if seen.
wait_for() {
    local log="$1" pat="$2" secs="$3" i=0
    while [ "$i" -lt "$((secs * 4))" ]; do
        grep -qE "$pat" "$log" 2>/dev/null && return 0
        sleep 0.25; i=$((i + 1))
    done
    return 1
}

# Run a node to completion, feeding cmds + a trailing `exit` so it quits at
# once (the kernel does not exit on stdin EOF; without `exit` each call would
# burn the whole timeout). timeout stays as a backstop.
run_once() {
    local img="$1" log="$2"; shift 2
    local script=""
    for c in "$@"; do script+="$c"$'\n'; done
    script+="exit"$'\n'
    printf '%s' "$script" | timeout 30 \
        env PKERNEL_PFS_BACKEND=ark PKERNEL_ARK_IMG="$img" "$KERNEL" \
        >"$log" 2>&1 || true
}

PUT_TEXT="hello-after-the-end-from-ark"

# ---------------------------------------------------------------------------
echo "==========================================================="
echo " ACT 1 — put -> on disk -> kill -9 (power loss) -> remount -> get"
echo "==========================================================="
IMG="$WORK/node.img"
L=/tmp/ark_be_act1_run1.log
echo "[act1] node stores a block into the ARK log, then we yank its power"
KPID=$(launch "$IMG" "$L" "pfs put $PUT_TEXT")
KPIDS+=("$KPID")
if wait_for "$L" "\[pfs\] put len=" 30; then
    ok "node confirmed the put (block appended + checkpointed to ARK)"
else
    bad "node never confirmed the put (see $L)"
fi
grep -qE "durable\(ark\): formatted image" "$L" \
    && ok "ARK backend selected + image formatted on first use" \
    || bad "ARK backend did not initialise (PKERNEL_PFS_BACKEND=ark)"

# Prove the bytes are physically in the ARK log on disk (raw, content-addressed).
if [ "$(grep -a -c "$PUT_TEXT" "$IMG" 2>/dev/null)" -ge 1 ]; then
    ok "the block's bytes are present in the ARK image ON DISK"
else
    bad "block bytes not found in the ARK image"
fi

echo "[act1] *** kill -9 $KPID  (simulated total power loss, no flush) ***"
kill -9 "$KPID" 2>/dev/null
sleep 1

L2=/tmp/ark_be_act1_run2.log
echo "[act1] reboot on the SAME ARK image — P0 is empty, get must fall through"
run_once "$IMG" "$L2" "pfs get $PUT_TEXT"
echo "----- remount + get log -----"
grep -E 'durable\(ark\)|\[pfs\] get' "$L2" | sed 's/^/    /'
echo "-----------------------------"

grep -qE "durable\(ark\): mounted image.* [1-9][0-9]* block" "$L2" \
    && ok "ARK image remounted with the block(s) in its log" \
    || bad "ARK did not remount with the block"
grep -qE "\[pfs\] get: $PUT_TEXT" "$L2" \
    && ok "pfs get served the block back FROM THE ARK LOG (sha256-verified)" \
    || bad "pfs get did NOT serve the block after remount"

# ---------------------------------------------------------------------------
echo
echo "==========================================================="
echo " ACT 2 — a corrupted ARK record is rejected on remount"
echo "==========================================================="
IMG2="$WORK/rot.img"
LC=/tmp/ark_be_act2_run1.log
ROT_TEXT="this-block-will-be-rotted-on-disk-ark"
run_once "$IMG2" "$LC" "pfs put $ROT_TEXT"
grep -qE "\[pfs\] put len=" "$LC" && ok "stored a block to corrupt" \
    || bad "could not store the block to corrupt"

echo "[act2] flip one byte inside the committed block's payload on disk"
python3 - "$IMG2" "$ROT_TEXT" <<'PY'
import sys
img, marker = sys.argv[1], sys.argv[2].encode()
d = bytearray(open(img, 'rb').read())
i = d.find(marker)
if i < 0:
    print("ERROR: marker not found in image"); sys.exit(2)
d[i + 5] ^= 0xFF                 # flip a payload byte (breaks crc + sha256)
open(img, 'wb').write(d)
print("    flipped byte at offset", i + 5)
PY
[ $? -eq 0 ] || bad "byte-flip injection failed"

LC2=/tmp/ark_be_act2_run2.log
run_once "$IMG2" "$LC2" "pfs get $ROT_TEXT"
echo "----- remount-with-corruption log -----"
grep -E 'durable\(ark\)|\[pfs\] get' "$LC2" | sed 's/^/    /'
echo "---------------------------------------"
grep -qE "durable\(ark\): mounted image" "$LC2" \
    && ok "the store still mounts (one rotted block does not kill the FS)" \
    || bad "the store failed to mount after corruption"
grep -qE "\[pfs\] get: NOT FOUND" "$LC2" \
    && ok "the rotted block is REJECTED (crc+sha self-verify), not served" \
    || bad "corruption was NOT detected — rotted bytes were served"

# ---------------------------------------------------------------------------
echo
echo "==========================================================="
echo " ACT 3 — crash-safety: the put path does not wedge the log"
echo "==========================================================="
IMG3="$WORK/crash.img"
LK=/tmp/ark_be_act3_run1.log
A_TEXT="committed-A-survives-kill9"
echo "[act3] put A, then kill -9 the live writer (power loss)"
KP=$(launch "$IMG3" "$LK" "pfs put $A_TEXT")
KPIDS+=("$KP")
wait_for "$LK" "\[pfs\] put len=" 30 && ok "A committed (checkpoint fsync'd)" \
    || bad "A never committed"
kill -9 "$KP" 2>/dev/null
sleep 1
LK2=/tmp/ark_be_act3_run2.log
B_TEXT="fresh-B-after-reboot"
echo "[act3] reboot: A must still be there AND a new put B must work"
run_once "$IMG3" "$LK2" "pfs get $A_TEXT" "pfs put $B_TEXT" "pfs get $B_TEXT"
grep -E '\[pfs\] get|\[pfs\] put' "$LK2" | sed 's/^/    /'
grep -qE "\[pfs\] get: $A_TEXT" "$LK2" \
    && ok "A survived the kill -9 (committed block is durable)" \
    || bad "A lost after crash"
grep -qE "\[pfs\] get: $B_TEXT" "$LK2" \
    && ok "a fresh put after reboot works (the log is not wedged)" \
    || bad "could not put/get after reboot — log wedged"

# ---------------------------------------------------------------------------
echo
if [ "$FAIL" = 0 ]; then
    echo "==========================================================="
    echo " RESULT: PASS — ARK is p-fs's durable backend."
    echo " A block put through p-fs lands in the ARK log, survives a"
    echo " kill -9, and is served back from the log; corruption is"
    echo " caught on remount. The filesystem and the content store"
    echo " are one thing."
    echo "==========================================================="
    exit 0
else
    echo "==========================================================="
    echo " RESULT: FAIL — see [FAIL] lines above and /tmp/ark_be_*.log"
    echo "==========================================================="
    exit 1
fi
