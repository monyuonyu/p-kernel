#!/bin/bash
# ---------------------------------------------------------------------------
# run_modver.sh — host cert for the MODULE-VERSION REGISTRY
#                 (arch/common/modver.{c,h} + galaxy GET /modules.json).
#
# mk_pino's directive: "each module carries a version, as fine-grained as
# possible", visible on the engineer page. This cert proves, end-to-end on
# the SHIPPED hosted binary, that:
#
#   [modver-table]   /modules.json on a live node is valid JSON, advertises a
#                    "build" id, and lists >= 15 modules — each {name,version}.
#   [modver-known]   the registry agrees with the modules' OWN header
#                    constants: swim == SWIM_VERSION, kdds == KDDS_VERSION,
#                    sign-manifest == SIGN_MANIFEST_VER, lm-self == LM_SELF_VER
#                    (read straight out of arch/common/include/*.h — if a
#                    header bumps but the registry doesn't, this FAILS).
#   [modver-engine]  the host-side inference engine (M1a-M1d, NS-1) appears:
#                    gguf / llm-quant / llm-forward / llm-tokenizer /
#                    ns-student are all present (their version is hosted-only).
#
# Pixels / the engineer-page rendering are NOT certified here — only the data
# plane (the JSON the page reads), matching samples/38_galaxy/galaxy_cert.sh.
# Non-flaky: end-state within a bound, never timing.
#
#   ./run_modver.sh            # exit 0 = all PASS, nonzero on any failure
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac
[ -x "$BOOT/p-kernel" ] || make -C "$BOOT" >/dev/null || { echo "[build] FAILED"; exit 1; }

INC="$ROOT/arch/common/include"
FAIL=0
PIDS=()
pass() { echo "$1 PASS"; }
fail() { echo "$1 FAIL: $2"; FAIL=1; }
killall_nodes() { for p in "${PIDS[@]:-}"; do kill -9 "$p" 2>/dev/null; done; PIDS=(); wait 2>/dev/null; }
trap killall_nodes EXIT
have_python() { command -v python3 >/dev/null 2>&1; }

# pull a `#define NAME  value` integer out of a header (first match).
hdr_def() {  # $1 = header file, $2 = macro name
    awk -v m="$2" '$1=="#define" && $2==m { print $3; exit }' "$1"
}

# ---- bring up ONE hosted node; /modules.json is hosted + loopback-bound ----
# Use a dedicated galaxy port (PKERNEL_GALAXY_PORT) so the cert never collides
# with a stale node squatting on the default 7800.
PORT="${MODVER_PORT:-7859}"
rm -f /tmp/modver_n1.log
PKERNEL_GALAXY_PORT="$PORT" PKERNEL_NODE_ID=1 "$BOOT/p-kernel" </dev/null >/tmp/modver_n1.log 2>&1 &
PIDS+=($!)
# bounded readiness: the galaxy server is up once the node has booted its
# tasks; poll the endpoint instead of timing.
J=""
t=0
while [ $t -lt 30 ]; do
    J="$(curl -s --max-time 5 "127.0.0.1:$PORT/modules.json" 2>/dev/null)"
    [ -n "$J" ] && break
    sleep 1; t=$((t+1))
done
echo "[modver] /modules.json: $J"

# -------------------------------------------------------- [modver-table] ----
if [ -z "$J" ]; then
    fail "[modver-table]" "no response from /modules.json"
else
    if have_python; then
        echo "$J" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null \
            || fail "[modver-table]" "/modules.json is not valid JSON"
    fi
    echo "$J" | grep -q '"build":"' || fail "[modver-table]" "no build id"
    # count the modules
    NMOD="$(echo "$J" | grep -o '"name":"' | wc -l)"
    echo "[modver] module count = $NMOD"
    if [ "$NMOD" -lt 15 ]; then
        fail "[modver-table]" "only $NMOD modules (<15)"
    fi
    [ "$FAIL" -eq 0 ] && pass "[modver-table]"
fi

# -------------------------------------------------------- [modver-known] ----
# assert the registry's version == the module's OWN header constant.
check_known() {  # $1 = json name, $2 = header file, $3 = macro
    local want got
    want="$(hdr_def "$INC/$2" "$3")"
    if [ -z "$want" ]; then fail "[modver-known]" "macro $3 not found in $2"; return; fi
    # extract the version that follows this name in the JSON
    got="$(echo "$J" | grep -o "\"name\":\"$1\",\"version\":[0-9]*" | grep -o '[0-9]*$')"
    if [ "$got" != "$want" ]; then
        fail "[modver-known]" "$1: registry=$got header($3)=$want"
    fi
}
KFAIL_BEFORE=$FAIL
check_known swim          swim.h        SWIM_VERSION
check_known kdds          kdds.h        KDDS_VERSION
check_known sign-manifest sign.h        SIGN_MANIFEST_VER
check_known lm-self       lm_self.h     LM_SELF_VER
check_known replica       replica.h     REPLICA_VERSION
[ "$FAIL" -eq "$KFAIL_BEFORE" ] && pass "[modver-known]"

# ------------------------------------------------------- [modver-engine] ----
EFAIL_BEFORE=$FAIL
for m in gguf llm-quant llm-forward llm-tokenizer ns-student; do
    echo "$J" | grep -q "\"name\":\"$m\"" || fail "[modver-engine]" "engine module '$m' absent"
done
[ "$FAIL" -eq "$EFAIL_BEFORE" ] && pass "[modver-engine]"

killall_nodes
echo ""
if [ "$FAIL" -eq 0 ]; then echo "[result] PASS"; else echo "[result] FAIL (see above)"; fi
exit $FAIL
