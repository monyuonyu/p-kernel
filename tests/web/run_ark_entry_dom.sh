#!/bin/bash
# tests/web/run_ark_entry_dom.sh — ark-entry.md 7.3 A8 and 7.2 4: the page's first
# screens (manifesto -> consent -> key backup -> heir -> first record) in headless
# Chromium against a real hosted node. The checks are in ark_entry_dom.py.
#
# Arms (what each must show is fixed before the screens were written):
#   plain          every A8-* PASS
#   nc-no-header   galaxy.html with the X-Galaxy-Token line of the fetch wrapper
#                  cut out: A8-header FAIL (the page is 401 everywhere)
#   nc-skip-backup galaxy.html whose entry starts on the heir step (the anchor
#                  /*AE-FIRST*/"key" -> "heir"): A8-order FAIL
# Added after the screens were written (the two above redden nearly every check, so
# they do not show that one check sees one fault):
#   nc-trim        the record text is .trim()ed before it is sent: A8-rec FAIL,
#                  A8-order A8-key A8-heir PASS
#   nc-keep-key    "written down" leaves the key in the DOM: A8-clear FAIL,
#                  A8-order A8-key A8-rec PASS
# The mutations must find their anchor exactly once, else the arm is RED (a moved
# anchor must not turn into a silent green).
# Needs python3 + playwright (+ a chromium: PK_CHROMIUM, or `playwright install
# chromium`). ARK_DOM_ARMS="plain" runs only the named arms (the rest are named in
# the RED line, so a partial run is never ALL PASS).
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
case "$(uname -m)" in
    aarch64|arm64) BOOT="$ROOT/boot/linux" ;;
    *)             BOOT="$ROOT/boot/linux_x86_64" ;;
esac
unset PKERNEL_RELAY PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN PKERNEL_AUTONET PKERNEL_GALAXY
PORT="${ARK_DOM_PORT:-17841}"
ARMS="${ARK_DOM_ARMS:-plain nc-no-header nc-skip-backup nc-trim nc-keep-key}"
SRC="$ROOT/arch/common/web/galaxy.html"
WORK="$(mktemp -d /tmp/arkdom.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
REDS=""

mutate() {  # <out> <anchor> <replacement>
    python3 - "$SRC" "$1" "$2" "$3" <<'EOF'
import sys
src, out, a, b = sys.argv[1:5]
t = open(src, encoding="utf-8").read()
n = t.count(a)
if n != 1:
    print("[ark-dom] mutate: anchor %r found %d times (want 1)" % (a, n)); sys.exit(1)
open(out, "w", encoding="utf-8").write(t.replace(a, b))
EOF
}
arm() {  # <name> <galaxy.html>
    case " $ARMS " in *" $1 "*) ;; *) REDS="$REDS $1-skipped"; return 1 ;; esac
    make -C "$BOOT" clean >/dev/null 2>&1
    if ! make -C "$BOOT" GALAXY_HTML="$2" > "$WORK/$1.build" 2>&1; then
        tail -20 "$WORK/$1.build"; echo "[ark-dom] $1 build FAIL"; REDS="$REDS $1-build"; return 1
    fi
    mkdir -p "$WORK/$1"
    timeout 600 python3 "$HERE/ark_entry_dom.py" "$BOOT/p-kernel" "$WORK/$1" "$PORT" > "$WORK/$1.out" 2>&1
    sed "s/^\[ark-dom\] /[ark-dom] $1 /" "$WORK/$1.out"
    for f in "$WORK/$1"/*/node.log; do [ -f "$f" ] && cp "$f" "/tmp/ark_dom_$1_$(basename "$(dirname "$f")").log"; done
}
is() { grep -q "^\[ark-dom\] $2 $3" "$WORK/$1.out"; }
want() {  # <arm> <PASS|FAIL> <check>...
    local a="$1" v="$2"; shift 2
    for c in "$@"; do is "$a" "$c" "$v" || REDS="$REDS $a-$c"; done
}

ALL="A8-url A8-order A8-key A8-clear A8-header A8-heir A8-rec A8-done A8-resume A8-skip"
# shellcheck disable=SC2086
if arm plain "$SRC"; then want plain PASS $ALL; fi
case " $ARMS " in *" nc-no-header "*)
    if mutate "$WORK/nohdr.html" 'h.set("X-Galaxy-Token",GX_TOKEN);' ''; then
        if arm nc-no-header "$WORK/nohdr.html"; then want nc-no-header FAIL A8-header; fi
    else REDS="$REDS nc-no-header-anchor"; fi ;;
*) REDS="$REDS nc-no-header-skipped" ;; esac
case " $ARMS " in *" nc-skip-backup "*)
    if mutate "$WORK/skip.html" '/*AE-FIRST*/"key"' '/*AE-FIRST*/"heir"'; then
        if arm nc-skip-backup "$WORK/skip.html"; then want nc-skip-backup FAIL A8-order; fi
    else REDS="$REDS nc-skip-backup-anchor"; fi ;;
*) REDS="$REDS nc-skip-backup-skipped" ;; esac
case " $ARMS " in *" nc-trim "*)
    if mutate "$WORK/trim.html" 'const t=document.getElementById("aerectext").value;' \
              'const t=document.getElementById("aerectext").value.trim();'; then
        if arm nc-trim "$WORK/trim.html"; then want nc-trim FAIL A8-rec; want nc-trim PASS A8-order A8-key A8-heir; fi
    else REDS="$REDS nc-trim-anchor"; fi ;;
*) REDS="$REDS nc-trim-skipped" ;; esac
case " $ARMS " in *" nc-keep-key "*)
    if mutate "$WORK/keep.html" 'if(r.ok){ document.getElementById("aekey").textContent=""; aeStep("heir"); }' \
              'if(r.ok){ aeStep("heir"); }'; then
        if arm nc-keep-key "$WORK/keep.html"; then want nc-keep-key FAIL A8-clear; want nc-keep-key PASS A8-order A8-key A8-rec; fi
    else REDS="$REDS nc-keep-key-anchor"; fi ;;
*) REDS="$REDS nc-keep-key-skipped" ;; esac
make -C "$BOOT" clean >/dev/null 2>&1
echo "[ark-dom] logs: /tmp/ark_dom_<arm>_<node>.log"
if [ -z "$REDS" ]; then echo "[ark-dom] ALL PASS"; exit 0; fi
echo "[ark-dom] RED:$REDS"; exit 1
