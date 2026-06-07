#!/bin/bash
# =============================================================================
# measure_locality.sh — does region locality actually reduce far-traffic?
# wave-12 F隊 / 俯瞰監査 v3 G25 (監査の死角: §4 を一度も「数」で測っていない)
# -----------------------------------------------------------------------------
# survival-network.md §4 の主張:
#   「MoE のスパース性 = region 局所ルーティング = 遠方通信を減らす =
#    光速とエネルギーの物理制約への答え」
# これが本当に効いているかを、対照実験で「数」にする。
#
# 対照 (同じ N=4, 同じ relay, 同じ推論列、唯一変えるのは region 粒度):
#   ON  : PKERNEL_RTT_ZONE_SIZE=2  → 2 region × 2 node (locality-aware)
#         DKVA の resp トピックは REGION スコープなので region 内に閉じる。
#   OFF : PKERNEL_RTT_ZONE_SIZE=4  → 1 個のフラット region (locality 無効)
#         全ノードが同 region とみなされ、REGION スコープも全員へ fan-out。
#
# 測るもの (既存観測点のみ; カーネルへは kdds.c に最小カウンタ1組のみ追加):
#   (a) kdds 配送数 (メッセージ数)  : `kdds` の [locality] 行 (near/far 内訳)
#   (b) kdds 配送バイト             : 同上
#   (c) relay 経由 DATA バイト/パケット : relay -v ログの "type=2 ... (N B)"
#   (d) 推論 wall-time              : node1 の dkva infer 前後の経過時間
#   (e) エネルギープロキシ          : near_bytes×1 + far_bytes×K (定義は下記)
#
# 目的は「主張が真でも偽でも、正直に数字を出す」こと。exit code ではなく
# レポートで語る。結果の考察は docs/benchmarks/locality.md。
#
# Usage: ./measure_locality.sh
# =============================================================================
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac

[ -x "$BOOT/p-kernel" ]    || make -C "$BOOT"        >/dev/null || exit 1
[ -x "$ROOT/relay/relay" ] || make -C "$ROOT/relay"  >/dev/null || exit 1

export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export PKERNEL_RELAY_HOST=127.0.0.1
export PKERNEL_RTT_ZONE_PENALTY=200

# Energy proxy weight for a "far" (cross-region) byte relative to a near byte.
# Long-haul transmission costs more energy roughly with distance/hops; we model
# the cross-region link as ZONE_PENALTY/TAU times costlier. tau=50ms (region
# threshold), penalty=200ms -> a cross-region byte ~= (50+200)/50 = 5 near bytes.
# This is an ORDER-OF-MAGNITUDE PROXY, not literal joules (see locality.md).
ENERGY_FAR_WEIGHT=5

TMP="$(mktemp -d /tmp/locality.XXXXXX)"
N_INFER=5

# -----------------------------------------------------------------------------
# run_config LABEL ZONE_SIZE PORT  -> appends one row to $TMP/results.tsv
# -----------------------------------------------------------------------------
run_config() {
    local label="$1" zone="$2" port="$3"
    local rl="$TMP/${label}_relay.log"
    local n1="$TMP/${label}_node1.log"
    local n2="$TMP/${label}_node2.log"
    local n3="$TMP/${label}_node3.log"
    local n4="$TMP/${label}_node4.log"
    local pids=()

    export PKERNEL_RELAY_PORT="$port"
    export PKERNEL_RTT_ZONE_SIZE="$zone"

    echo "[run:$label] zone_size=$zone penalty=$PKERNEL_RTT_ZONE_PENALTY port=$port"
    "$ROOT/relay/relay" -p "$port" -v >"$rl" 2>&1 & pids+=($!)
    sleep 1

    # peers (2,3,4): settle through the whole burst, then dump their own
    # cumulative kdds counters (for the cluster sum) and exit. The burst on
    # node1 runs ~t=16..26 (13s settle + 5*2s); peers print kdds at ~t=30,
    # after the burst but before the final kill (~t=37), so the cluster sum
    # captures all inference traffic. Counters are cumulative so node1 exiting
    # first does not lose anything.
    { sleep 30; echo "kdds"; sleep 1; echo "exit"; } | \
        PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >"$n2" 2>&1 & pids+=($!)
    { sleep 30; echo "kdds"; sleep 1; echo "exit"; } | \
        PKERNEL_NODE_ID=3 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >"$n3" 2>&1 & pids+=($!)
    { sleep 30; echo "kdds"; sleep 1; echo "exit"; } | \
        PKERNEL_NODE_ID=4 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >"$n4" 2>&1 & pids+=($!)
    sleep 2

    # node1 = requester/driver. Snapshot kdds before & after the inference burst,
    # and wall-clock the burst itself.
    {
        sleep 13                       # let SWIM measure RTT and regions settle
        echo "region"                  # show how nodes split into regions
        echo "kdds"                    # SNAPSHOT-A (before inference burst)
        echo "__BURST_START__"
        for i in $(seq 1 $N_INFER); do
            echo "dkva infer $((40 + i*3)) $((10 + i)) $((70 + i*2)) $((3 + i))"
            sleep 2
        done
        echo "__BURST_END__"
        echo "kdds"                    # SNAPSHOT-B (after inference burst)
        sleep 1
        echo "exit"
    } | PKERNEL_NODE_ID=1 PKERNEL_AUTONET=1 "$BOOT/p-kernel" >"$n1" 2>&1

    sleep 10     # let peers print their cumulative kdds counters first
    kill "${pids[@]}" 2>/dev/null
    wait 2>/dev/null

    parse_run "$label" "$rl" "$n1" "$n2" "$n3" "$n4"
}

# -----------------------------------------------------------------------------
# Field extractor for a "[locality] tx_msgs=.. far_msgs=.. near_msgs=.. ..." line
#   loc_field <logfile> <which:first|last> <key>
# -----------------------------------------------------------------------------
loc_field() {
    local f="$1" which="$2" key="$3"
    local line
    if [ "$which" = "first" ]; then
        line="$(grep '\[locality\]' "$f" 2>/dev/null | head -1)"
    else
        line="$(grep '\[locality\]' "$f" 2>/dev/null | tail -1)"
    fi
    [ -z "$line" ] && { echo 0; return; }
    echo "$line" | grep -oE "${key}=[0-9]+" | head -1 | grep -oE '[0-9]+'
}

parse_run() {
    local label="$1" rl="$2" n1="$3" n2="$4" n3="$5" n4="$6"

    # --- node1 requester: before/after delta around the inference burst -------
    local a_msgs a_far a_nb a_fb b_msgs b_far b_nb b_fb
    a_msgs=$(loc_field "$n1" first tx_msgs);  b_msgs=$(loc_field "$n1" last tx_msgs)
    a_far=$(loc_field "$n1" first far_msgs);  b_far=$(loc_field "$n1" last far_msgs)
    a_nb=$(loc_field "$n1" first near_bytes); b_nb=$(loc_field "$n1" last near_bytes)
    a_fb=$(loc_field "$n1" first far_bytes);  b_fb=$(loc_field "$n1" last far_bytes)
    local d_msgs=$((b_msgs - a_msgs))
    local d_far=$((b_far - a_far))
    local d_near=$((d_msgs - d_far))
    local d_nb=$((b_nb - a_nb))
    local d_fb=$((b_fb - a_fb))

    # --- cluster cumulative: sum every node's final [locality] line ----------
    local c_msgs=0 c_far=0 c_nb=0 c_fb=0 f v
    for f in "$n1" "$n2" "$n3" "$n4"; do
        v=$(loc_field "$f" last tx_msgs);    c_msgs=$((c_msgs + v))
        v=$(loc_field "$f" last far_msgs);   c_far=$((c_far + v))
        v=$(loc_field "$f" last near_bytes); c_nb=$((c_nb + v))
        v=$(loc_field "$f" last far_bytes);  c_fb=$((c_fb + v))
    done
    local c_near=$((c_msgs - c_far))

    # --- relay: total DATA (type=4) packets and bytes through the relay ------
    # type=1=REGISTER, type=3=KEEPALIVE; the pmesh/kdds payload rides type=4
    # (dst=0 broadcast). Beacon overhead is in here too but is identical ON/OFF,
    # so the ON-vs-OFF delta still reflects the kdds (inference) traffic.
    local r_pkts r_bytes
    r_pkts=$(grep -c 'rx v[0-9]* type=4' "$rl" 2>/dev/null); r_pkts=${r_pkts:-0}
    r_bytes=$(grep 'rx v[0-9]* type=4' "$rl" 2>/dev/null \
              | grep -oE '\([0-9]+ B\)' | grep -oE '[0-9]+' \
              | awk '{s+=$1} END{print s+0}')

    # --- wall-time of the inference burst (node1 stderr/stdout has no clock,
    #     so derive it from the driver: N_INFER * 2s sleeps is the floor; the
    #     real signal is whether the SIM injects extra delay. It does NOT —
    #     RTT zone penalty only inflates *reported* RTT for region formation,
    #     never a real usleep — so wall-time is identical ON vs OFF by design.
    #     We still record the count of completed inferences as a liveness check.
    local done_infer
    done_infer=$(grep -c '=> class\|aggregated\|\[dkva' "$n1" 2>/dev/null)
    done_infer=${done_infer:-0}

    # --- energy proxy: near_bytes*1 + far_bytes*K (cluster cumulative) -------
    local e_proxy=$((c_nb + c_fb * ENERGY_FAR_WEIGHT))

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$label" "$d_msgs" "$d_near" "$d_far" "$d_nb" "$d_fb" \
        "$c_msgs" "$c_near" "$c_far" "$c_nb" "$c_fb" \
        "$r_pkts" "$r_bytes" "$e_proxy" "$done_infer" >> "$TMP/results.tsv"
}

# -----------------------------------------------------------------------------
echo "==========================================================================="
echo " locality measurement — §4 controlled experiment (N=4, ${N_INFER} dkva infers)"
echo " host=$(uname -m)  boot=$BOOT"
echo "==========================================================================="

run_config "ON"  2 7461   # locality-aware: 2 regions of 2
run_config "OFF" 4 7462   # flat: 1 region of 4 (locality disabled)

# -----------------------------------------------------------------------------
# Report
# -----------------------------------------------------------------------------
echo
echo "===== RESULT: node1 requester, per inference-burst (${N_INFER} infers) ====="
printf '%-5s | %8s %8s %8s | %10s %10s\n' \
    cfg msgs near far near_B far_B
printf -- '------+-------------------------------+-----------------------\n'
while IFS=$'\t' read -r label dm dn df dnb dfb cm cn cf cnb cfb rp rb ep di; do
    printf '%-5s | %8s %8s %8s | %10s %10s\n' \
        "$label" "$dm" "$dn" "$df" "$dnb" "$dfb"
done < "$TMP/results.tsv"

echo
echo "===== RESULT: cluster cumulative (all 4 nodes) + relay + energy proxy ====="
printf '%-5s | %8s %8s %8s | %10s %8s %10s | %12s\n' \
    cfg msgs near far near_B far_B relayDATA_B E_proxy
printf -- '------+-------------------------------+----------------------------------+-------------\n'
while IFS=$'\t' read -r label dm dn df dnb dfb cm cn cf cnb cfb rp rb ep di; do
    printf '%-5s | %8s %8s %8s | %10s %8s %10s | %12s\n' \
        "$label" "$cm" "$cn" "$cf" "$cnb" "$cfb" "$rb" "$ep"
done < "$TMP/results.tsv"

echo
echo "===== §4 verdict (computed, honest) ====="
# Compare cluster total messages ON vs OFF.
on=$(grep -P '^ON\t'  "$TMP/results.tsv");  off=$(grep -P '^OFF\t' "$TMP/results.tsv")
on_msgs=$(echo "$on"  | cut -f7);  off_msgs=$(echo "$off" | cut -f7)
on_nb=$(echo "$on"    | cut -f10); on_fb=$(echo "$on"    | cut -f11)
on_ep=$(echo "$on"    | cut -f14); off_ep=$(echo "$off"  | cut -f14)
on_rb=$(echo "$on"    | cut -f13); off_rb=$(echo "$off"  | cut -f13)
echo " total kdds messages: ON=$on_msgs  OFF=$off_msgs"
if [ "${on_msgs:-0}" -lt "${off_msgs:-0}" ] 2>/dev/null; then
    echo " -> locality REDUCES total kdds messages: §4 (traffic) SUPPORTED by the numbers."
elif [ "${on_msgs:-0}" -gt "${off_msgs:-0}" ] 2>/dev/null; then
    echo " -> locality INCREASES total messages: §4 (traffic) NOT supported here."
else
    echo " -> no difference in total messages: §4 (traffic) inconclusive."
fi
echo " relay DATA bytes:    ON=$on_rb  OFF=$off_rb"
echo " energy proxy:        ON=$on_ep  OFF=$off_ep   (near_B*1 + far_B*${ENERGY_FAR_WEIGHT})"
echo " NOTE: ON keeps DKVA 'resp' partials inside a region (REGION scope);"
echo "       OFF collapses all 4 nodes into one region so 'resp' fans out to all."
echo " NOTE: 'far' is defined relative to region boundaries. OFF has one flat"
echo "       region, so far=0 BY DEFINITION even though the RTT model still says"
echo "       those nodes are distant — that masking is exactly the non-locality"
echo "       design §4 warns against. Read total/relay bytes, not far alone."
echo " NOTE: the RTT zone penalty only inflates *reported* RTT for region"
echo "       formation; it injects NO real per-packet delay, so wall-time does"
echo "       not move ON vs OFF. The light-speed half of §4 is MODELLED, not"
echo "       measured here (see docs/benchmarks/locality.md TODO for real fleet)."
echo
echo "[done] raw logs + results.tsv under $TMP"
