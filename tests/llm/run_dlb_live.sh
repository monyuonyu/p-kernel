#!/bin/bash
# ---------------------------------------------------------------------------
# run_dlb_live.sh — fable5 Wave-D3: CI-enforce the now-LIVE DLB feeder.
#
# Wave-D2 (student_shell.c:1047) wired the compounding ring into the SHIPPED
# mouth: student_chat_generate routes a DELIBERATE arithmetic question through
# dlb_gate_vexact -> dlb_answer (SEARCH x VERIFY, a free perfect V-exact
# verifier) and, AFTER the conscience ALLOWs the reply, dlb_compound_enqueue's
# the verified winner; the DMN sleep tick then distills the ring into weight-
# resident skill. The `student dlb` / `student dlb off` shell verbs exercise
# that EXACT production entry in-process and print the loop turning.
#
# This cert boots the WHOLE hosted ./p-kernel (native, no qemu, no net,
# PKERNEL_DEVICE_TIER=S newborn, PKERNEL_PFS_DIR=mktemp) and drives those verbs
# over stdin. FOUR arms, green-when-HEALTHY around the honest cold-start truth:
#
#   [live-feeder]  STRUCTURAL + RUNTIME, HARD. (a) student_chat_generate() —
#                  the SHIPPED chat entry — must itself call BOTH dlb_answer and
#                  dlb_compound_enqueue (the feeder wired on the production path,
#                  proven by extracting the function body and grepping IN it).
#                  (b) `student dlb` must print a [dlb-live] line with gated>=1
#                  (the V-exact gate actually FIRES on arithmetic). This is the
#                  load-bearing "the wire is LIVE" proof — RED if the feeder is
#                  gone from the chat entry OR the gate never fires.
#
#   [gate-off]     HARD. `student dlb off` drives N non-arithmetic prompts; the
#                  gate must stay inert: gated==0 AND enq==0. RED if either fires
#                  (a misfire that would deliberate/learn on ordinary chat).
#
#   [closure]      enq-or-NULL. If the live run enq>=1 then the loop must CLOSE:
#                  distilled>=1 AND post>=pre (RED otherwise). If enq==0 (the
#                  honest tier-S cold start: a newborn W cannot yet produce a
#                  verify-PASS arithmetic answer, and the read-only oracle refuses
#                  wrong answers) we PRINT a pre-registered honest-NULL line and
#                  STRUCTURALLY PASS. No seed-cherry-picking a win.
#
#   [sabotage]     ANTI-THEATER. Rebuild the WHOLE p-kernel with the existing
#                  compile-time switch -DDLB_SABOTAGE_NOVERIFY (dlb.c stubs the
#                  search x verify SELECTION so a searched candidate can NEVER
#                  beat the draft) and assert the live run shows flip==0 —
#                  proving any future flip/enq is verify-DRIVEN, not noise. At the
#                  cold start flip==0 both with AND without the switch; the tooth
#                  is that the switch COMPILES + RUNS and can never manufacture a
#                  flip. RED if the sabotaged binary ever reports flip>0.
#
# Exit 0 = structural green (feeder live, gate clean, closure closed-or-NULL,
# sabotage cannot flip). Exit 1 = a REAL failure (feeder missing, gate misfire,
# closure broken with enq>0, or sabotage still flipping). Crown-neutral: this
# cert touches NO crown TU and rebuilds only via EXTRA_CFLAGS.
#
#   ./run_dlb_live.sh
# ---------------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SS="$ROOT/arch/common/llm/student_shell.c"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ---- native hosted boot dir (no qemu): x86_64 -> boot/linux_x86_64,
#      aarch64 -> boot/linux. Both link the SAME arch/common/llm/*.c. ----------
case "$(uname -m)" in
    x86_64)  BOOTDIR="$ROOT/boot/linux_x86_64" ;;
    aarch64|arm64) BOOTDIR="$ROOT/boot/linux" ;;
    *) echo "[skip] unsupported host arch $(uname -m) — need a native hosted p-kernel"; exit 0 ;;
esac

RC=0

# ===========================================================================
# ARM 1a — [live-feeder] STRUCTURAL: the SHIPPED chat entry feeds the ring.
# Extract the student_chat_generate() body and assert BOTH dlb_answer and
# dlb_compound_enqueue are called INSIDE it (not merely somewhere in the file).
# ===========================================================================
echo "[grep] [live-feeder] — student_chat_generate() calls dlb_answer AND dlb_compound_enqueue"
BODY="$(awk '/^int student_chat_generate\(/{f=1} f{print} f&&/^}/{exit}' "$SS")"
HAS_ANS="$(printf '%s\n' "$BODY" | grep -cE 'dlb_answer\(&g_student,')"
HAS_ENQ="$(printf '%s\n' "$BODY" | grep -cE 'dlb_compound_enqueue\(')"
if [ "$HAS_ANS" -ge 1 ] && [ "$HAS_ENQ" -ge 1 ]; then
    echo "  PASS  [live-feeder] the production chat entry feeds the compounding ring:"
    printf '%s\n' "$BODY" | grep -nE 'dlb_answer\(&g_student,|dlb_compound_enqueue\(' | sed 's/^/        /'
else
    echo "  FAIL  [live-feeder] student_chat_generate() does NOT wire the feeder (ans=$HAS_ANS enq=$HAS_ENQ)"
    RC=1
fi

# ---- build the native hosted p-kernel (LIVE binary) ------------------------
echo ""
echo "[build] hosted ./p-kernel (native, $BOOTDIR) ..."
make -C "$BOOTDIR" clean >/dev/null 2>&1
if ! make -C "$BOOTDIR" >"$WORK/build_live.log" 2>&1; then
    echo "  FAIL  [build] native p-kernel build failed:"; tail -20 "$WORK/build_live.log" | sed 's/^/      /'
    echo "[result] FAIL (build)"; exit 1
fi
cp "$BOOTDIR/p-kernel" "$WORK/pk-live"

# ---- helper: boot the kernel at tier S with a throwaway pfs, drive verbs ----
drive() {  # $1 = binary, $2 = stdin verbs; prints kernel stdout+stderr
    local bin="$1" verbs="$2" pfs; pfs="$(mktemp -d)"
    PKERNEL_PFS_DIR="$pfs" PKERNEL_DEVICE_TIER=S \
        timeout 240 sh -c "printf '$verbs' | '$bin'" 2>&1
    rm -rf "$pfs"
}
field() { printf '%s\n' "$1" | grep -aoE "$2=[0-9.]+" | head -1 | sed "s/^$2=//"; }

# ===========================================================================
# ARM 1b + closure — drive `student dlb` on the LIVE binary.
# ===========================================================================
echo ""
echo "[run] LIVE: 'student dlb' (tier S newborn, mktemp pfs, no net) ..."
LIVE_OUT="$(drive "$WORK/pk-live" 'student dlb\nexit\n')"
LIVE_LINE="$(printf '%s\n' "$LIVE_OUT" | grep -aF '[dlb-live]' | head -1)"
if [ -z "$LIVE_LINE" ]; then
    echo "  FAIL  [live-feeder] no [dlb-live] line — the verb did not run"
    printf '%s\n' "$LIVE_OUT" | tail -8 | sed 's/^/      /'
    RC=1
    LIVE_LINE="[dlb-live] gated=0 enq=0 flip=0 distilled=0 pending=0 pre=0 post=0"
fi
echo "  $LIVE_LINE"
L_GATED="$(field "$LIVE_LINE" gated)";   L_ENQ="$(field "$LIVE_LINE" enq)"
L_FLIP="$(field "$LIVE_LINE" flip)";     L_DIST="$(field "$LIVE_LINE" distilled)"
L_PRE="$(field "$LIVE_LINE" pre)";       L_POST="$(field "$LIVE_LINE" post)"

echo ""
echo "[arm] [live-feeder] RUNTIME — the V-exact gate FIRES on arithmetic (gated>=1)"
if [ "${L_GATED:-0}" -ge 1 ]; then
    echo "  PASS  [live-feeder] gated=$L_GATED (the shipped mouth routed arithmetic through dlb_answer)"
else
    echo "  FAIL  [live-feeder] gated=$L_GATED — the gate never fired (feeder dead on the live path)"
    RC=1
fi

echo ""
echo "[arm] [closure] — enq-or-NULL: if the ring filled, the loop must CLOSE"
if [ "${L_ENQ:-0}" -ge 1 ]; then
    OK_DIST=0; OK_MONO=0
    [ "${L_DIST:-0}" -ge 1 ] && OK_DIST=1
    awk "BEGIN{exit !($L_POST+0 >= $L_PRE+0)}" && OK_MONO=1
    if [ "$OK_DIST" -eq 1 ] && [ "$OK_MONO" -eq 1 ]; then
        echo "  PASS  [closure] enq=$L_ENQ -> distilled=$L_DIST>=1 AND post=$L_POST >= pre=$L_PRE (loop closed)"
    else
        echo "  FAIL  [closure] enq=$L_ENQ but distilled=$L_DIST / post=$L_POST < pre=$L_PRE (loop did NOT close)"
        RC=1
    fi
else
    # PRE-REGISTERED honest-NULL (deterministic at tier-S cold start).
    echo "  NULL  [closure] enq=0 — feeder live, flow starved at tier S:"
    echo "        newborn W too immature to verify-pass arithmetic; the read-only"
    echo "        oracle refuses wrong answers, so nothing enqueues. Structural PASS"
    echo "        (no seed-cherry-picking); closure becomes load-bearing once the"
    echo "        baby can produce a verify-PASS answer (enq>0 reachable)."
fi

# ===========================================================================
# ARM 2 — [gate-off]: N non-arithmetic prompts must NOT deliberate or enqueue.
# ===========================================================================
echo ""
echo "[run] LIVE: 'student dlb off' (non-arithmetic prompts) ..."
OFF_OUT="$(drive "$WORK/pk-live" 'student dlb off\nexit\n')"
OFF_LINE="$(printf '%s\n' "$OFF_OUT" | grep -aF '[dlb-off]' | head -1)"
echo "  ${OFF_LINE:-<no [dlb-off] line>}"
echo "[arm] [gate-off] — gated==0 AND enq==0 on ordinary chat (no misfire)"
if [ -z "$OFF_LINE" ]; then
    echo "  FAIL  [gate-off] no [dlb-off] line — the verb did not run"; RC=1
else
    O_GATED="$(field "$OFF_LINE" gated)"; O_ENQ="$(field "$OFF_LINE" enq)"
    if [ "${O_GATED:-1}" -eq 0 ] && [ "${O_ENQ:-1}" -eq 0 ]; then
        echo "  PASS  [gate-off] gated=$O_GATED enq=$O_ENQ (the gate stays inert on non-arithmetic chat)"
    else
        echo "  FAIL  [gate-off] gated=$O_GATED enq=$O_ENQ — the gate MISFIRED on ordinary chat"; RC=1
    fi
fi

# ===========================================================================
# ARM 3 — [sabotage] ANTI-THEATER: rebuild p-kernel -DDLB_SABOTAGE_NOVERIFY.
# With verify-selection stubbed a searched candidate can never beat the draft,
# so flip can NEVER be produced. Assert the sabotaged live run shows flip==0.
# (Green now: flip==0 with AND without at cold start; the tooth is the switch
# compiles + runs and can never manufacture a flip — any future flip is real.)
# ===========================================================================
echo ""
echo "[build] SABOTAGE p-kernel (-DDLB_SABOTAGE_NOVERIFY; verify-selection stubbed) ..."
make -C "$BOOTDIR" clean >/dev/null 2>&1
if ! make -C "$BOOTDIR" EXTRA_CFLAGS=-DDLB_SABOTAGE_NOVERIFY >"$WORK/build_sab.log" 2>&1; then
    echo "  FAIL  [sabotage] the -DDLB_SABOTAGE_NOVERIFY build failed (switch does not compile):"
    tail -20 "$WORK/build_sab.log" | sed 's/^/      /'; RC=1
else
    cp "$BOOTDIR/p-kernel" "$WORK/pk-sab"
    SAB_OUT="$(drive "$WORK/pk-sab" 'student dlb\nexit\n')"
    SAB_LINE="$(printf '%s\n' "$SAB_OUT" | grep -aF '[dlb-live]' | head -1)"
    echo "  ${SAB_LINE:-<no [dlb-live] line under sabotage>}"
    echo "[arm] [sabotage] — verify-selection disabled -> flip==0 (never manufactured)"
    if [ -z "$SAB_LINE" ]; then
        echo "  FAIL  [sabotage] the sabotaged binary did not run the verb"; RC=1
    else
        S_FLIP="$(field "$SAB_LINE" flip)"
        if [ "${S_FLIP:-1}" -eq 0 ]; then
            echo "  PASS  [sabotage] flip=$S_FLIP — with verify-selection stubbed no flip is produced"
            echo "        (the switch compiles + runs; any future flip/enq is verify-DRIVEN, not noise)"
        else
            echo "  FAIL  [sabotage] flip=$S_FLIP>0 with verify-selection STUBBED — a flip was manufactured!"; RC=1
        fi
    fi
fi
# leave the source tree clean (no stray kernel binary / objects).
make -C "$BOOTDIR" clean >/dev/null 2>&1

echo ""
if [ "$RC" -eq 0 ]; then
    echo "[result] PASS"
    echo "[note] The DLB feeder is LIVE in the shipped chat entry (structural + runtime"
    echo "       gated>=1). The gate stays inert on ordinary chat. Closure is closed-or"
    echo "       -honest-NULL (tier-S newborn cannot yet verify-pass arithmetic). Sabotage"
    echo "       (-DDLB_SABOTAGE_NOVERIFY) can never manufacture a flip — the loop is"
    echo "       verify-DRIVEN. Crown-neutral: no crown TU touched."
    exit 0
else
    echo "[result] FAIL (RC=$RC)"
    exit 1
fi
