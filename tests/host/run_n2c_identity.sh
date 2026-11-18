#!/bin/bash
# tests/host/run_n2c_identity.sh — N2c first slice: the node keeps its key and
# its incarnation across restarts (docs/architecture/30-module/research/n2c-identity.md
# §2 1 and 4, §3 I1 I2, audit-183 conditions in §5).
#
# Expectations fixed before the code (2026-11-19):
#   I1  a node with PKERNEL_PFS_DIR and no PKERNEL_NODE_ID: `sign key` says
#       "[sign] key <64 hex> src=new id=<n>" on the first start and
#       "src=disk" with the same key and the same id on the second; node.key is
#       32 bytes, mode 600
#   I1e without PKERNEL_PFS_DIR the key is "src=ephemeral" and differs between
#       two starts (nothing to keep it in; the old behaviour)
#   I2  node.key cut to 5 bytes: the next start says one line
#       "[sign] node.key: ..." , keeps the 5 bytes as node.key.bad, and node.key
#       is a new 32-byte key
#   I2b cut again while node.key.bad is there: nothing is overwritten (node.key
#       and node.key.bad keep their bytes), the run uses "src=ephemeral" and says so
#   I2s node_seed cut to 5 bytes: one line "[node-id] node_seed: ...", the
#       5 bytes kept as node_seed.bad, a new node_seed of 32 hex chars
#   IN  with the net up (a relay port nothing listens on), the first start says
#       "[swim] incarnation 1 (node.incarn)" and the second "incarnation 2"
# Falsifiers (each must turn its check red; the plain build must be ALL PASS):
#   -DN2C_NC_KEY_EPHEMERAL -> I1    -DN2C_NC_KEY_OVERWRITE -> I2
#   -DN2C_NC_SEED_REMINT   -> I2s   -DN2C_NC_INC_RESET     -> IN
# Output: "[n2c-id] <build> <check> PASS|FAIL", then ALL PASS (exit 0) or RED.
# Rebuilds boot/linux* with EXTRA_CFLAGS and cleans it afterwards.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
unset PKERNEL_RELAY PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_AUTONET \
      PKERNEL_NODE_ID PKERNEL_NODE_SEED PKERNEL_PFS_DIR
if [ -n "${N2C_WORK:-}" ]; then          # keep the logs (N2C_WORK=<empty dir>)
    WORK="$N2C_WORK"; mkdir -p "$WORK"
    trap 'make -C "$BOOT" clean >/dev/null 2>&1' EXIT
else
    WORK="$(mktemp -d /tmp/n2cid.XXXXXX)"
    trap 'rm -rf "$WORK"; make -C "$BOOT" clean >/dev/null 2>&1' EXIT
fi
PORT="${N2C_RELAY_PORT:-7493}"   # nothing listens there: the net comes up, nobody answers
MODES="${N2C_MODES:-plain N2C_NC_KEY_EPHEMERAL N2C_NC_KEY_OVERWRITE N2C_NC_SEED_REMINT N2C_NC_INC_RESET}"

build() {  # <cflags>
    make -C "$BOOT" clean >/dev/null 2>&1
    make -C "$BOOT" EXTRA_CFLAGS="$1" > "$WORK/build.log" 2>&1
}
run() {  # <dir|-> <log> <net 0|1> <commands...>
    local d="$1" log="$2" nt="$3"; shift 3
    printf '%s\n' "$@" exit > "$WORK/in.txt"
    if [ "$d" = - ]; then
        timeout 60 "$BOOT/p-kernel" < "$WORK/in.txt" > "$log" 2>&1
    elif [ "$nt" = 1 ]; then
        timeout 60 env PKERNEL_PFS_DIR="$d" PKERNEL_RELAY="127.0.0.1:$PORT" \
            PKERNEL_RELAY_AUTOFALLBACK=0 PKERNEL_AUTONET=1 "$BOOT/p-kernel" < "$WORK/in.txt" > "$log" 2>&1
    else
        timeout 60 env PKERNEL_PFS_DIR="$d" "$BOOT/p-kernel" < "$WORK/in.txt" > "$log" 2>&1
    fi
}
keyline() {  # <log>: the first "[sign] key" line, \r removed
    grep -a '^\[sign\] key ' "$1" | head -1 | tr -d '\r'
}
keyhex() { keyline "$1" | sed -n 's/^\[sign\] key \([0-9a-f]\{64\}\) .*/\1/p'; }
keysrc() { keyline "$1" | sed -n 's/.* src=\([a-z]*\).*/\1/p'; }
keyid()  { keyline "$1" | sed -n 's/.* id=\([0-9]*\).*/\1/p'; }
fsize()  { if [ -e "$1" ]; then wc -c < "$1" | tr -d ' '; else echo none; fi; }

suite() {  # <tag> <cflags>: prints the checks, sets SUITE_REDS
    local tag="$1" cf="$2" reds="" d de ds dn k1 k2 b1 b2
    r() { echo "[n2c-id] $tag $1 $2"; [ "$2" = FAIL ] && reds="$reds $1"; return 0; }
    if ! build "$cf"; then
        r build FAIL; tail -20 "$WORK/build.log"; SUITE_REDS="build"; return
    fi
    # I1
    d="$WORK/$tag-d"; mkdir -p "$d"
    run "$d" "$WORK/$tag-a.log" 0 "sign key"
    run "$d" "$WORK/$tag-b.log" 0 "sign key"
    k1=$(keyhex "$WORK/$tag-a.log"); k2=$(keyhex "$WORK/$tag-b.log")
    echo "[n2c-id] $tag I1 detail: 1st $(keysrc "$WORK/$tag-a.log") id=$(keyid "$WORK/$tag-a.log") ${k1:0:16} / 2nd $(keysrc "$WORK/$tag-b.log") id=$(keyid "$WORK/$tag-b.log") ${k2:0:16} / node.key $(fsize "$d/node.key") B mode $(stat -c %a "$d/node.key" 2>/dev/null)"
    if [ "${#k1}" = 64 ] && [ "$k1" = "$k2" ] && [ "$(keysrc "$WORK/$tag-a.log")" = new ] \
       && [ "$(keysrc "$WORK/$tag-b.log")" = disk ] && [ -n "$(keyid "$WORK/$tag-a.log")" ] \
       && [ "$(keyid "$WORK/$tag-a.log")" = "$(keyid "$WORK/$tag-b.log")" ] \
       && [ "$(fsize "$d/node.key")" = 32 ] && [ "$(stat -c %a "$d/node.key")" = 600 ]; then
        r I1 PASS; else r I1 FAIL; fi
    # I1e
    run - "$WORK/$tag-e1.log" 0 "sign key"
    run - "$WORK/$tag-e2.log" 0 "sign key"
    b1=$(keyhex "$WORK/$tag-e1.log"); b2=$(keyhex "$WORK/$tag-e2.log")
    if [ "${#b1}" = 64 ] && [ "${#b2}" = 64 ] && [ "$b1" != "$b2" ] \
       && [ "$(keysrc "$WORK/$tag-e1.log")" = ephemeral ]; then r I1e PASS; else r I1e FAIL; fi
    # I2: cut node.key to 5 bytes
    printf 'abcde' > "$d/node.key"
    run "$d" "$WORK/$tag-c.log" 0 "sign key"
    k2=$(keyhex "$WORK/$tag-c.log")
    echo "[n2c-id] $tag I2 detail: src=$(keysrc "$WORK/$tag-c.log") node.key $(fsize "$d/node.key") B node.key.bad $(fsize "$d/node.key.bad") B lines $(grep -ac '^\[sign\] node.key: ' "$WORK/$tag-c.log")"
    if [ "$(grep -ac '^\[sign\] node.key: ' "$WORK/$tag-c.log")" = 1 ] \
       && [ "$(cat "$d/node.key.bad" 2>/dev/null)" = abcde ] && [ "$(fsize "$d/node.key")" = 32 ] \
       && [ "${#k2}" = 64 ] && [ "$k2" != "$k1" ] && [ "$(keysrc "$WORK/$tag-c.log")" = new ]; then
        r I2 PASS; else r I2 FAIL; fi
    # I2b: cut again while node.key.bad is there
    printf 'vwxyz' > "$d/node.key"
    run "$d" "$WORK/$tag-c2.log" 0 "sign key"
    if [ "$(grep -ac '^\[sign\] node.key: ' "$WORK/$tag-c2.log")" = 1 ] \
       && [ "$(cat "$d/node.key" 2>/dev/null)" = vwxyz ] && [ "$(cat "$d/node.key.bad" 2>/dev/null)" = abcde ] \
       && [ "$(keysrc "$WORK/$tag-c2.log")" = ephemeral ]; then r I2b PASS; else r I2b FAIL; fi
    # I2s: node_seed cut to 5 bytes
    ds="$WORK/$tag-ds"; mkdir -p "$ds"
    run "$ds" "$WORK/$tag-s1.log" 0 "sign key"
    printf 'abcde' > "$ds/node_seed"
    run "$ds" "$WORK/$tag-s2.log" 0 "sign key"
    echo "[n2c-id] $tag I2s detail: node_seed $(fsize "$ds/node_seed") B node_seed.bad $(fsize "$ds/node_seed.bad") B lines $(grep -ac '^\[node-id\] node_seed: ' "$WORK/$tag-s2.log")"
    if [ "$(grep -ac '^\[node-id\] node_seed: ' "$WORK/$tag-s2.log")" -ge 1 ] \
       && [ "$(cat "$ds/node_seed.bad" 2>/dev/null)" = abcde ] && [ "$(fsize "$ds/node_seed")" = 32 ]; then
        r I2s PASS; else r I2s FAIL; fi
    # IN: incarnation kept and bumped
    dn="$WORK/$tag-dn"; mkdir -p "$dn"
    run "$dn" "$WORK/$tag-n1.log" 1 "nodes"
    run "$dn" "$WORK/$tag-n2.log" 1 "nodes"
    echo "[n2c-id] $tag IN detail: $(grep -a '^\[swim\] incarnation' "$WORK/$tag-n1.log" | tr -d '\r') / $(grep -a '^\[swim\] incarnation' "$WORK/$tag-n2.log" | tr -d '\r') / transport $(grep -ac '^\[net\] transport = ' "$WORK/$tag-n2.log")"
    if grep -aq '^\[swim\] incarnation 1 (node.incarn)' "$WORK/$tag-n1.log" \
       && grep -aq '^\[swim\] incarnation 2 (node.incarn)' "$WORK/$tag-n2.log"; then r IN PASS; else r IN FAIL; fi
    SUITE_REDS="$reds"
}

FAIL=0
for m in $MODES; do
    if [ "$m" = plain ]; then
        suite plain ""
        [ -z "$SUITE_REDS" ] || { echo "[n2c-id] plain red:$SUITE_REDS"; FAIL=1; }
        continue
    fi
    case "$m" in
        N2C_NC_KEY_EPHEMERAL) want=I1 ;;
        N2C_NC_KEY_OVERWRITE) want=I2 ;;
        N2C_NC_SEED_REMINT)   want=I2s ;;
        N2C_NC_INC_RESET)     want=IN ;;
        *) echo "[n2c-id] unknown mode $m"; FAIL=1; continue ;;
    esac
    suite "$m" "-D$m"
    case " $SUITE_REDS " in
        *" $want "*) echo "[n2c-id] $m turns $want red (as it must)" ;;
        *) echo "[n2c-id] $m does NOT turn $want red"; FAIL=1 ;;
    esac
done
if [ "$FAIL" = 0 ]; then echo "[n2c-id] ALL PASS"; exit 0; fi
echo "[n2c-id] RED"; exit 1
