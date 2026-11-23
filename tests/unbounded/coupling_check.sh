#!/bin/sh
# coupling_check.sh — the [unbounded-coupling] falsifier (audit correction 1).
# Args: three sizeprobe outputs built at DNODE_MAX = 64 / 256 / 1024.
# PASS iff every R-sized quantity is byte-for-byte CONSTANT across the three
# wires AND the wire (DNODE_MAX) strictly GROWS. Either a moving R-column (a
# fleet term leaked into an R structure) or a flat wire (the probe did not
# actually widen) is RED.
set -u
O64="$1"; O256="$2"; O1024="$3"
field() { tr ' ' '\n' < "$1" | grep -a "^$2=" | cut -d= -f2; }

fails=0
# R-column: MUST be identical across all three wires.
for k in DREGION_MAX KDDS_TOPIC_MAX kdds_topics_bytes dkva_preopen nodemap_bytes; do
    v64=$(field "$O64" "$k"); v256=$(field "$O256" "$k"); v1024=$(field "$O1024" "$k")
    if [ "$v64" = "$v256" ] && [ "$v256" = "$v1024" ]; then
        echo "  R-const  $k = $v64 (flat: 64==256==1024)  OK"
    else
        echo "  RED: R-sized $k MOVED with the wire: $v64 / $v256 / $v1024"
        fails=$((fails+1))
    fi
done
# Fleet-column: the wire MUST have widened (proves the probe really built
# 64/256/1024). Until N3 2b this was dnode_table_bytes growing; 2b moved the
# rows into the SWIM slots (hosted: 0 bytes), so the evidence is DNODE_MAX
# itself (research/n3-dnode-region.md 2.4).
d64=$(field "$O64" DNODE_MAX)
d256=$(field "$O256" DNODE_MAX)
d1024=$(field "$O1024" DNODE_MAX)
if [ "$d64" -lt "$d256" ] && [ "$d256" -lt "$d1024" ]; then
    echo "  fleet    DNODE_MAX = $d64 -> $d256 -> $d1024 (GREW: wire widened)  OK"
else
    echo "  RED: the wire did not widen (DNODE_MAX $d64/$d256/$d1024)"
    fails=$((fails+1))
fi
echo "  info     dnode_table_bytes = $(field "$O64" dnode_table_bytes) / $(field "$O256" dnode_table_bytes) / $(field "$O1024" dnode_table_bytes) (hosted: 0 since N3 2b)"

if [ "$fails" -eq 0 ]; then
    echo "  [unbounded-coupling]  PASS — per-node R-cost is CONSTANT while fleet N (wire) grows"
    exit 0
fi
echo "  [unbounded-coupling]  FAIL ($fails)"
exit 1
