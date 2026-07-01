#!/bin/bash
# ---------------------------------------------------------------------------
# i18n_cert.sh — the falsifiable gate for the i18n manifesto wave
# (docs/architecture/30-module/ark-profile.md §7.5). The manifesto speaks many
# languages; consent binds to the EXACT words the person READ. curl-driven,
# non-flaky (end-state, not a timing window). One tag:
#
#   [i18n-manifesto]  every embedded language serves with a self-consistent
#                     content-id (curl /manifesto?lang=xx; sha256 of the
#                     served bytes == the advertised X-Manifesto-Id); the
#                     embedded-language COUNT is printed; an ack with a
#                     NON-ja mid (Spanish) unlocks teach (the consent-id
#                     TABLE works, and the stored manifesto_id is the
#                     Spanish id — honest per-language); a bogus ?lang=
#                     falls back cleanly (X-Manifesto-Lang=en, still 200);
#                     /langs lists each code with its endonym.
#
# NO identity verification anywhere (§3.3). Pixels are NOT certified. Logs:
# /tmp/i18n40_*. Exit non-zero on any failed assertion.
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
# de-flake helper (crown-neutral): curl000 retries ONLY a literal transient
# (HTTP 000 / curl exit 7,28,52,56 = no-response), bounded + FAIL-CLOSED; a real
# code (the 403/200 consent gates, the sha256/X-Manifesto-Lang/ok:true checks)
# returns immediately and is NEVER retried. wait_http is a boot-readiness gate.
# See samples/lib/http_retry.sh.
. "$HERE/../lib/http_retry.sh"

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
fail() { echo "[i18n-manifesto] FAIL: $*"; FAIL=1; }
killall_nodes() { for p in "${PIDS[@]:-}"; do kill -9 "$p" 2>/dev/null; done; PIDS=(); wait 2>/dev/null; sleep 1; }
trap killall_nodes EXIT

PORT=7800
hdr_val() { grep -i "$1" "$2" | tr -d '\r' | awk '{print $2}'; }

D=$(mktemp -d /tmp/i18n40.XXXXXX)
PKERNEL_NODE_ID=1 PKERNEL_PFS_DIR="$D" "$BOOT/p-kernel" </dev/null \
    >/tmp/i18n40_node.log 2>&1 & PIDS+=($!)
sleep 3
wait_http "$PORT" /langs || { fail "node never became ready (HTTP 000)"; killall_nodes; rm -rf "$D"; exit 1; }

# (1) /langs is JSON with at least ja + en + es + zh-Hans, endonyms present.
LANGS=$(curl000 -s --max-time 8 "127.0.0.1:$PORT/langs")
for k in '"ja":"日本語"' '"en":"English"' '"es":"Español"' '"zh-Hans":"简体中文"'; do
    echo "$LANGS" | grep -qF "$k" || { fail "/langs missing $k"; killall_nodes; rm -rf "$D"; exit 1; }
done
# count the codes in /langs (number of top-level "code": keys)
COUNT=$(echo "$LANGS" | grep -o '"[A-Za-z-]*":"' | wc -l)
log "embedded languages: $COUNT"
[ "$COUNT" -ge 30 ] || { fail "embedded language count $COUNT < 30"; killall_nodes; rm -rf "$D"; exit 1; }

# (2) for EVERY code in /langs, sha256(served bytes) == advertised id.
CODES=$(echo "$LANGS" | grep -o '"[A-Za-z-]*":' | tr -d '":' )
NOK=0
for c in $CODES; do
    H=$(mktemp); B=$(mktemp)
    curl000 -s -D "$H" -o "$B" --max-time 8 "127.0.0.1:$PORT/manifesto?lang=$c" >/dev/null
    ADV=$(hdr_val 'X-Manifesto-Id' "$H")
    SRV=$(hdr_val 'X-Manifesto-Lang' "$H")
    GOT=$(sha256sum "$B" | awk '{print $1}')
    if [ -z "$ADV" ] || [ "$ADV" != "$GOT" ]; then
        fail "lang $c: served bytes sha256 $GOT != advertised $ADV"; rm -f "$H" "$B"; killall_nodes; rm -rf "$D"; exit 1
    fi
    [ "$SRV" = "$c" ] || { fail "lang $c: X-Manifesto-Lang=$SRV"; rm -f "$H" "$B"; killall_nodes; rm -rf "$D"; exit 1; }
    rm -f "$H" "$B"; NOK=$((NOK+1))
done
log "all $NOK languages: sha256(served) == advertised X-Manifesto-Id"

# (3) bogus ?lang= falls back cleanly: 200 + X-Manifesto-Lang=en.
H=$(mktemp)
BCODE=$(curl000 -s -D "$H" -o /dev/null -w '%{http_code}' --max-time 8 "127.0.0.1:$PORT/manifesto?lang=zz-NOPE")
BSRV=$(hdr_val 'X-Manifesto-Lang' "$H"); rm -f "$H"
[ "$BCODE" = "200" ] || { fail "bogus lang not 200 ($BCODE)"; killall_nodes; rm -rf "$D"; exit 1; }
[ "$BSRV" = "en" ]   || { fail "bogus lang fallback X-Manifesto-Lang=$BSRV (want en)"; killall_nodes; rm -rf "$D"; exit 1; }
log "bogus ?lang= -> 200, fell back to en"

# (4) Accept-Language auto-default (no ?lang): fr-FR -> fr.
H=$(mktemp)
curl000 -s -D "$H" -o /dev/null --max-time 8 -H 'Accept-Language: fr-FR,fr;q=0.9,en;q=0.5' \
    "127.0.0.1:$PORT/manifesto" >/dev/null
ALSRV=$(hdr_val 'X-Manifesto-Lang' "$H"); rm -f "$H"
[ "$ALSRV" = "fr" ] || { fail "Accept-Language fr-FR did not auto-default to fr ($ALSRV)"; killall_nodes; rm -rf "$D"; exit 1; }
log "Accept-Language fr-FR -> fr (auto-default)"

# (5) the consent TABLE: teach 403 before, ack with the SPANISH mid unlocks
#     teach, and the stored manifesto_id IS the Spanish id (honest).
C1=$(curl000 -s -o /dev/null -w '%{http_code}' --max-time 8 -d 'k=sun&v=yellow' 127.0.0.1:$PORT/teach)
[ "$C1" = "403" ] || { fail "teach before consent not 403 ($C1)"; killall_nodes; rm -rf "$D"; exit 1; }
H=$(mktemp)
curl000 -s -D "$H" -o /dev/null --max-time 8 "127.0.0.1:$PORT/manifesto?lang=es" >/dev/null
ESID=$(hdr_val 'X-Manifesto-Id' "$H"); rm -f "$H"
PR=$(curl000 -s -w '\n%{http_code}' --max-time 8 -d "ack=1&mid=$ESID" 127.0.0.1:$PORT/profile)
PCODE=$(echo "$PR" | tail -1)
echo "$PR" | grep -q '"ok":true' || { fail "ack with Spanish mid not ok ($PCODE)"; killall_nodes; rm -rf "$D"; exit 1; }
C2=$(curl000 -s -o /dev/null -w '%{http_code}' --max-time 8 -d 'k=sun&v=yellow' 127.0.0.1:$PORT/teach)
[ "$C2" = "200" ] || { fail "teach after Spanish ack not 200 ($C2)"; killall_nodes; rm -rf "$D"; exit 1; }
PJ=$(curl000 -s --max-time 8 127.0.0.1:$PORT/profile.json)
echo "$PJ" | grep -q "\"manifesto_id\":\"$ESID\"" || { fail "stored manifesto_id != Spanish id (consent not per-language)"; killall_nodes; rm -rf "$D"; exit 1; }
log "Spanish (non-ja) mid unlocked teach; stored manifesto_id == Spanish id"

killall_nodes; rm -rf "$D"
echo "------------------------------------------------------------"
if [ $FAIL -eq 0 ]; then
    echo "[i18n-manifesto] PASS ($NOK languages, consent-id table works)"
else
    echo "[i18n-manifesto] FAILURES ABOVE"
fi
exit $FAIL
