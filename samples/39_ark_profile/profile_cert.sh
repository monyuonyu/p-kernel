#!/bin/bash
# ---------------------------------------------------------------------------
# profile_cert.sh — the falsifiable data-plane gate for ark-profile v1
# (docs/architecture/ark-profile.md §8). 人類の記憶 — the human chapter of
# the autobiography. curl-driven, non-flaky (end-state within a bound, never
# a timing window). Three tags:
#
#   [ark-consent]     the 共感 gate is enforced AND consent != disclosure:
#                     teach is 403 before any profile; the served manifesto
#                     bytes hash to X-Manifesto-Id (consent binds to the
#                     EXACT words); an ack-ONLY (no disclosure) profile
#                     unlocks teach; a WRONG mid is 409 and leaves teach 403.
#   [ark-profile]     the declaration is real, chained, and death-piercing:
#                     a full profile hash-verifies (profile.json id ==
#                     sha256 of the canonical struct via `pfs cat`); the
#                     self/lin head's human_ref == that id; the profile
#                     SURVIVES a restart on the same PKERNEL_PFS_DIR; an
#                     edit appends seq+1 and `pfs log self/prof` length 2.
#   [ark-provenance]  a taught fact's provenance resolves to the profile:
#                     web teach -> ARK_PROV src=1 + profile_head==profile id;
#                     a SHELL teach -> newest record src=0. One write site,
#                     both mouths (§5).
#
# NO identity verification anywhere (§3.3: 誰もそれを検証しません) — the
# cert never asserts any uniqueness/email/key check, only the gates above.
# Pixels are NOT certified (galaxy.md §7 / ark-profile.md §9). Logs: /tmp/ark39_*
# Exit non-zero if any assertion fails.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    x86_64|amd64)  BOOT="$ROOT/boot/linux_x86_64" ;;
    *) echo "unsupported host arch $(uname -m)"; exit 1 ;;
esac
[ -x "$BOOT/p-kernel" ] || make -C "$BOOT" >/dev/null || exit 1

FAIL=0
PIDS=()
TS()   { date '+%H:%M:%S'; }
log()  { echo "[$(TS)] $*"; }
pass() { echo "$1 PASS"; }
fail() { echo "$1 FAIL: $2"; FAIL=1; }

killall_nodes() { for p in "${PIDS[@]:-}"; do kill -9 "$p" 2>/dev/null; done; PIDS=(); wait 2>/dev/null; sleep 1; }
trap killall_nodes EXIT

sha256_of() { sha256sum "$1" | awk '{print $1}'; }

# fetch the served manifesto bytes + X-Manifesto-Id; echo "<id> <bytesfile>"
fetch_manifesto() {
    local port="$1" hdr body
    hdr=$(mktemp); body=$(mktemp)
    curl -s -D "$hdr" -o "$body" --max-time 5 "127.0.0.1:$port/manifesto" >/dev/null
    local id
    id=$(grep -i 'X-Manifesto-Id' "$hdr" | tr -d '\r' | awk '{print $2}')
    rm -f "$hdr"
    echo "$id $body"
}

# ----------------------------------------------------------------- consent
gate_consent() {
    log "--- [ark-consent]: the 共感 gate + consent != disclosure ---"
    local D; D=$(mktemp -d /tmp/ark39_consent.XXXXXX)
    PKERNEL_NODE_ID=1 PKERNEL_PFS_DIR="$D" "$BOOT/p-kernel" </dev/null \
        >/tmp/ark39_consent.log 2>&1 & PIDS+=($!)
    sleep 3

    # (1) teach BEFORE any profile -> 403 + "manifesto"
    local R code
    R=$(curl -s -w '\n%{http_code}' --max-time 5 -d 'k=2&v=3' 127.0.0.1:7800/teach)
    code=$(echo "$R" | tail -1)
    [ "$code" = "403" ] || { fail "[ark-consent]" "teach before profile not 403 ($code)"; killall_nodes; rm -rf "$D"; return; }
    echo "$R" | grep -q 'manifesto' || { fail "[ark-consent]" "403 body missing 'manifesto'"; killall_nodes; rm -rf "$D"; return; }

    # (2) served manifesto bytes hash == advertised X-Manifesto-Id
    read -r MID MBYTES < <(fetch_manifesto 7800)
    local H; H=$(sha256_of "$MBYTES"); rm -f "$MBYTES"
    [ -n "$MID" ] || { fail "[ark-consent]" "no X-Manifesto-Id header"; killall_nodes; rm -rf "$D"; return; }
    [ "$H" = "$MID" ] || { fail "[ark-consent]" "served bytes hash $H != X-Manifesto-Id $MID"; killall_nodes; rm -rf "$D"; return; }
    log "manifesto id $MID == sha256(served bytes)"

    # (3) ack-ONLY profile (NO disclosure fields) -> {ok}
    R=$(curl -s -w '\n%{http_code}' --max-time 5 -d "ack=1&mid=$MID" 127.0.0.1:7800/profile)
    code=$(echo "$R" | tail -1)
    echo "$R" | grep -q '"ok":true' || { fail "[ark-consent]" "ack-only profile not ok ($code)"; killall_nodes; rm -rf "$D"; return; }

    # (4) the same teach now -> 200 (consent != disclosure: ack-only unlocks)
    R=$(curl -s -w '\n%{http_code}' --max-time 5 -d 'k=2&v=3' 127.0.0.1:7800/teach)
    code=$(echo "$R" | tail -1)
    [ "$code" = "200" ] || { fail "[ark-consent]" "teach after ack-only not 200 ($code)"; killall_nodes; rm -rf "$D"; return; }
    echo "$R" | grep -q '"ok":true' || { fail "[ark-consent]" "teach after ack not ok"; killall_nodes; rm -rf "$D"; return; }
    killall_nodes; rm -rf "$D"

    # (5) negative control: a FRESH node + WRONG mid -> 409, teach still 403
    local D2; D2=$(mktemp -d /tmp/ark39_consent2.XXXXXX)
    PKERNEL_NODE_ID=1 PKERNEL_PFS_DIR="$D2" "$BOOT/p-kernel" </dev/null \
        >/tmp/ark39_consent2.log 2>&1 & PIDS+=($!)
    sleep 3
    local WRONG="0000000000000000000000000000000000000000000000000000000000000000"
    R=$(curl -s -w '\n%{http_code}' --max-time 5 -d "ack=1&mid=$WRONG" 127.0.0.1:7800/profile)
    code=$(echo "$R" | tail -1)
    [ "$code" = "409" ] || { fail "[ark-consent]" "wrong mid not 409 ($code)"; killall_nodes; rm -rf "$D2"; return; }
    R=$(curl -s -w '\n%{http_code}' --max-time 5 -d 'k=2&v=3' 127.0.0.1:7800/teach)
    code=$(echo "$R" | tail -1)
    [ "$code" = "403" ] || { fail "[ark-consent]" "teach still gated expected 403 after wrong mid ($code)"; killall_nodes; rm -rf "$D2"; return; }
    killall_nodes; rm -rf "$D2"
    pass "[ark-consent]"
}

# ----------------------------------------------------------------- profile
gate_profile() {
    log "--- [ark-profile]: real, chained, death-piercing ---"
    local D; D=$(mktemp -d /tmp/ark39_prof.XXXXXX)
    local F; F=$(mktemp -u /tmp/ark39_fifo.XXXXXX); mkfifo "$F"
    exec 5<>"$F"
    PKERNEL_NODE_ID=1 PKERNEL_PFS_DIR="$D" "$BOOT/p-kernel" <"$F" \
        >/tmp/ark39_prof.log 2>&1 & local NODE=$!; PIDS+=($NODE)
    sleep 3

    read -r MID MBYTES < <(fetch_manifesto 7800); rm -f "$MBYTES"
    # full profile: handle=cert_h + a 未来への言葉
    local R; R=$(curl -s --max-time 5 -d "ack=1&mid=$MID&handle=cert_h&msg=hello+future" 127.0.0.1:7800/profile)
    log "profile: $R"
    echo "$R" | grep -q '"ok":true' || { fail "[ark-profile]" "full profile not ok"; exec 5>&-; killall_nodes; rm -rf "$D" "$F"; return; }
    local PID; PID=$(echo "$R" | grep -o '"id":"[0-9a-f]*"' | head -1 | grep -o '[0-9a-f]\{64\}')

    # (a) profile.json: seq + handle reflected
    local PJ; PJ=$(curl -s --max-time 5 127.0.0.1:7800/profile.json)
    echo "$PJ" | grep -q '"seq":1' || { fail "[ark-profile]" "profile.json seq != 1"; exec 5>&-; killall_nodes; rm -rf "$D" "$F"; return; }
    echo "$PJ" | grep -q "\"id\":\"$PID\"" || { fail "[ark-profile]" "profile.json id mismatch"; exec 5>&-; killall_nodes; rm -rf "$D" "$F"; return; }

    # (b) hash-verify: `pfs cat self/prof` bytes sha256 == the advertised id
    : > /tmp/ark39_prof.mark; printf 'pfs cat self/prof\n' >&5; sleep 2
    # extract the 1188 struct bytes after "cat 'self/prof' seq=1: " from the log
    python3 - "$PID" <<'PY' || { fail "[ark-profile]" "pfs cat self/prof hash mismatch"; exec 5>&-; killall_nodes; rm -rf "$D" "$F"; return; }
import sys, hashlib
pid = sys.argv[1]
data = open("/tmp/ark39_prof.log","rb").read()
mark = b"cat 'self/prof' seq=1: "
i = data.rfind(mark)
assert i >= 0, "no cat self/prof line"
start = i + len(mark)
blob = data[start:start+1188]
assert len(blob) == 1188, "short profile blob %d" % len(blob)
h = hashlib.sha256(blob).hexdigest()
assert h == pid, "sha256(self/prof bytes) %s != profile.json id %s" % (h, pid)
print("profile bytes hash-verify OK:", h)
PY

    # (c) self.json head human_ref == the profile id (linked into self/lin)
    local SJ; SJ=$(curl -s --max-time 5 127.0.0.1:7800/self.json)
    echo "$SJ" | grep -q "\"human_ref\":\"$PID\"" || { fail "[ark-profile]" "self/lin head human_ref != profile id"; exec 5>&-; killall_nodes; rm -rf "$D" "$F"; return; }

    # (e) edit-by-append: a second profile -> seq 2, `pfs log self/prof` len 2
    R=$(curl -s --max-time 5 -d "ack=1&mid=$MID&handle=cert_h2" 127.0.0.1:7800/profile)
    echo "$R" | grep -q '"seq":2' || { fail "[ark-profile]" "edit did not bump seq to 2"; exec 5>&-; killall_nodes; rm -rf "$D" "$F"; return; }
    printf 'pfs log self/prof\n' >&5; sleep 2
    # the log walks newest-first; count seq= lines under the last log header
    python3 - <<'PY' || { fail "[ark-profile]" "pfs log self/prof length != 2"; exec 5>&-; killall_nodes; rm -rf "$D" "$F"; return; }
data = open("/tmp/ark39_prof.log","rb").read().decode("latin-1")
i = data.rfind("log 'self/prof'")
assert i >= 0, "no log self/prof"
tail = data[i:]
# count distinct version seq markers the log prints (seq=1 and seq=2)
import re
seqs = set(re.findall(r"seq=(\d+)", tail[:4000]))
assert "1" in seqs and "2" in seqs, "log did not show both versions: %r" % seqs
print("pfs log self/prof shows versions:", sorted(seqs))
PY

    # (d) restart: same PKERNEL_PFS_DIR -> identical seq/handle/id
    local BEFORE_SEQ BEFORE_ID
    BEFORE_ID=$(curl -s --max-time 5 127.0.0.1:7800/profile.json | grep -o '"id":"[0-9a-f]\{64\}"' | head -1)
    BEFORE_SEQ=$(curl -s --max-time 5 127.0.0.1:7800/profile.json | grep -o '"seq":[0-9]*' | head -1)
    exec 5>&-; kill -9 "$NODE" 2>/dev/null; wait 2>/dev/null; sleep 1
    PKERNEL_NODE_ID=1 PKERNEL_PFS_DIR="$D" "$BOOT/p-kernel" </dev/null \
        >/tmp/ark39_prof_restart.log 2>&1 & PIDS+=($!)
    sleep 3
    local AFTER; AFTER=$(curl -s --max-time 5 127.0.0.1:7800/profile.json)
    echo "$AFTER" | grep -q "$BEFORE_ID" || { fail "[ark-profile]" "restart lost profile id ($BEFORE_ID)"; killall_nodes; rm -rf "$D" "$F"; return; }
    echo "$AFTER" | grep -q "$BEFORE_SEQ" || { fail "[ark-profile]" "restart lost profile seq"; killall_nodes; rm -rf "$D" "$F"; return; }
    echo "$AFTER" | grep -q '"handle_len":7' || { fail "[ark-profile]" "restart lost handle (cert_h2)"; killall_nodes; rm -rf "$D" "$F"; return; }
    log "restart survived: $BEFORE_SEQ $BEFORE_ID"
    killall_nodes; rm -rf "$D" "$F"
    pass "[ark-profile]"
}

# -------------------------------------------------------------- provenance
gate_provenance() {
    log "--- [ark-provenance]: who taught this resolves to the profile ---"
    local D; D=$(mktemp -d /tmp/ark39_prov.XXXXXX)
    local F; F=$(mktemp -u /tmp/ark39_pfifo.XXXXXX); mkfifo "$F"
    exec 5<>"$F"
    PKERNEL_NODE_ID=1 PKERNEL_PFS_DIR="$D" "$BOOT/p-kernel" <"$F" \
        >/tmp/ark39_prov.log 2>&1 & PIDS+=($!)
    sleep 3

    read -r MID MBYTES < <(fetch_manifesto 7800); rm -f "$MBYTES"
    local R; R=$(curl -s --max-time 5 -d "ack=1&mid=$MID&handle=cert_h" 127.0.0.1:7800/profile)
    local PID; PID=$(echo "$R" | grep -o '"id":"[0-9a-f]\{64\}"' | head -1 | grep -o '[0-9a-f]\{64\}')

    # a WEB teach -> ARK_PROV src=1 (k=2 v=3), profile_head == PID
    curl -s --max-time 5 -d 'k=2&v=3' 127.0.0.1:7800/teach >/dev/null
    sleep 1
    printf 'pfs cat self/prov\n' >&5; sleep 2
    python3 - "$PID" <<'PY' || { fail "[ark-provenance]" "web teach prov record wrong"; exec 5>&-; killall_nodes; rm -rf "$D" "$F"; return; }
import sys, struct, binascii
pid = sys.argv[1]
data = open("/tmp/ark39_prov.log","rb").read()
mark = b"cat 'self/prov' seq="
i = data.rfind(mark)
assert i >= 0, "no cat self/prov"
# skip "seq=N: "
j = data.index(b": ", i) + 2
blob = data[j:j+48]
assert len(blob) == 48, "short prov %d" % len(blob)
magic, fseq, key, val, origin, src, age = struct.unpack_from("<IIBBBBI", blob, 0)
phead = binascii.hexlify(blob[16:48]).decode()
assert magic == 0x564F5250, "bad magic"
assert key == 2 and val == 3, "key/val %d/%d != 2/3" % (key, val)
assert src == 1, "web teach src %d != 1" % src
assert phead == pid, "profile_head %s != profile id %s" % (phead, pid)
print("web prov: key=%d val=%d src=%d profile_head==id OK" % (key, val, src))
PY

    # a SHELL teach -> newest prov record src=0
    printf 'mind teach 5 1\n' >&5; sleep 2
    printf 'pfs cat self/prov\n' >&5; sleep 2
    python3 - <<'PY' || { fail "[ark-provenance]" "shell teach prov src != 0"; exec 5>&-; killall_nodes; rm -rf "$D" "$F"; return; }
import struct
data = open("/tmp/ark39_prov.log","rb").read()
mark = b"cat 'self/prov' seq="
i = data.rfind(mark)
j = data.index(b": ", i) + 2
blob = data[j:j+48]
magic, fseq, key, val, origin, src, age = struct.unpack_from("<IIBBBBI", blob, 0)
assert magic == 0x564F5250, "bad magic"
assert key == 5 and val == 1, "shell teach key/val %d/%d != 5/1" % (key, val)
assert src == 0, "shell teach src %d != 0" % src
print("shell prov: key=%d val=%d src=%d (one write site, both mouths)" % (key, val, src))
PY
    exec 5>&-; killall_nodes; rm -rf "$D" "$F"
    pass "[ark-provenance]"
}

gate_consent
gate_profile
gate_provenance

echo "------------------------------------------------------------"
if [ $FAIL -eq 0 ]; then echo "profile_cert: ALL GATES PASS"; else echo "profile_cert: FAILURES ABOVE"; fi
exit $FAIL
