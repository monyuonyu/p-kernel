#!/bin/bash
# ---------------------------------------------------------------------------
# emulator_smoke.sh — RUNTIME smoke test for the shipped UMP Android APK
#                      (fable5 audit gap G1, ranked highest risk).
#
# WHY: ci.yml's `android-apk-build` job only proves the APK COMPILES + LINKS
# (assembleDebug + "does app-debug.apk exist" — see its own header comment).
# Nothing in CI has ever EXECUTED it. That gap let two runtime-only breakages
# ship with CI green in the same week:
#   4141d325 — PKernelService.onStartCommand silently skipped bootKernelOnce()
#              when the battery/WiFi run-gate was false, even on a genuine
#              user tap (fixed by the EXTRA_USER_EXPLICIT gate-pierce this
#              script deliberately exercises below).
#   90feb043 — the consent/manifesto CSS died (a WebView/HTML-rendering bug;
#              out of scope for this script, which asserts kernel-boot +
#              galaxy-HTTP health, not pixel-level page rendering).
#
# WHAT THIS PROVES (all via the REAL, shipped JNI/Kotlin/native code paths,
# never a mock):
#   1. The debug APK installs on a real emulator.
#   2. Launching MainActivity with NO extras (the default first-light path
#      every real user takes) boots the native T-Kernel thread
#      (pkernel_jni.c nativeBoot -> arch/linux/aarch64/usermain.c) even when
#      the phone reports a low, unplugged battery -- i.e. the
#      EXTRA_USER_EXPLICIT gate-pierce (MainActivity.startKernelWith /
#      PKernelService.onStartCommand) still fires. A CI emulator normally
#      reports a full/AC battery, so runAllowed() alone would be true and a
#      regression in the pierce logic would go UNNOTICED -- this script
#      forces the exact gate condition 4141d325 broke under.
#   3. The in-kernel galaxy HTTP server (arch/common/galaxy.c) answers on its
#      loopback port with the real boot console output AND a structurally
#      valid /galaxy.json.
#
# BOOT MARKER: arch/linux/aarch64/usermain.c:900 prints
#   "T-Kernel is alive inside a Linux process."
# via print() -> sio_send_frame() -> console_ring_note() (arch/linux/aarch64/
# sio.c:89-92), which arch/common/galaxy.c's GET /console.txt route serves
# straight out of that ring (console_ring.c; _TK_HOSTED_LIBC_-gated, and IS
# defined for the Android build -- see CMakeLists.txt:55). This is a BETTER
# signal than `adb logcat`: the kernel's stdout is captured by pkernel_jni.c
# into an in-process pipe for the in-app LogActivity UI (PKernelService.kt's
# appendLog ring) and is NEVER routed through android.util.Log, so it does
# NOT appear in logcat at all. /console.txt is the one channel that carries
# it out over the wire, unmodified, for exactly this kind of remote/CI check.
#
# NODE ID / PORT: the galaxy port is 7800 + (node_id - 1) (galaxy.c:94,1597).
# A real install derives a RANDOM stable node id in [1..63] on first launch
# (MainActivity.ensureNodeId, the N-0 wave), so the exact port is NOT known
# ahead of time on a fresh emulator. This script forwards + polls the whole
# 7800-7862 range rather than assuming node 1 / port 7800.
#
# Usage:
#   ./emulator_smoke.sh [path/to/app-debug.apk]
#   Must run against an already-booted, adb-reachable emulator/device (this
#   is the `script:` body of the reactivecircus/android-emulator-runner step
#   in ci.yml's proposed android-emulator-smoke job). Exits 0 on PASS.
# ---------------------------------------------------------------------------
set -u

APK="${1:-android/app/build/outputs/apk/debug/app-debug.apk}"
PKG="io.pkernel.ump"                    # applicationId (build.gradle.kts)
ACTIVITY="io.pkernel.MainActivity"      # namespace=io.pkernel + MainActivity.kt
PORT_LO=7800
PORT_HI=7862                            # 7800 + (DNODE_MAX-1 max id 63) - 1
BOOT_TIMEOUT_S=60
BOOT_MARKER="T-Kernel is alive inside a Linux process."
SHELL_PROMPT="p-kernel> "

DIAG=0
fail() { echo "  FAIL  $1: $2"; DIAG=1; }
have_python() { command -v python3 >/dev/null 2>&1; }

echo "[emulator-smoke] APK=$APK"
test -f "$APK" || { echo "[emulator-smoke] missing $APK -- build it first (gradlew assembleDebug)"; exit 1; }

adb wait-for-device
echo "[emulator-smoke] installing (adb install -r -g)"
adb install -r -g "$APK" || { echo "[emulator-smoke] adb install FAILED"; exit 1; }

# --- Force the exact gate condition 4141d325 broke under --------------------
# PKernelService.powerAllowed(): battery <= floor (default 30%) AND unplugged
# holds the boot UNLESS the launching intent carries EXTRA_USER_EXPLICIT=true.
# MainActivity's default (no-extras) launch path calls startKernelWith(), which
# always sets that extra -- so a regression that drops (or never sets) the
# pierce would silently strand the kernel exactly as 4141d325 did, and this
# smoke would time out below instead of going unnoticed.
echo "[emulator-smoke] forcing battery LOW+unplugged (exercises the EXTRA_USER_EXPLICIT gate-pierce)"
adb shell dumpsys battery unplug         >/dev/null 2>&1
adb shell dumpsys battery set ac 0       >/dev/null 2>&1
adb shell dumpsys battery set usb 0      >/dev/null 2>&1
adb shell dumpsys battery set wireless 0 >/dev/null 2>&1
adb shell dumpsys battery set status 3   >/dev/null 2>&1   # BATTERY_STATUS_DISCHARGING
adb shell dumpsys battery set level 10   >/dev/null 2>&1   # well under the 30% floor

adb shell wm dismiss-keyguard >/dev/null 2>&1 || true

echo "[emulator-smoke] am start -n $PKG/$ACTIVITY (default no-extras path)"
adb shell am start -W -n "$PKG/$ACTIVITY" || { echo "[emulator-smoke] am start FAILED"; exit 1; }

# forward the whole node-id port range ONCE; harmless no-ops on ports nobody
# ever binds (adb forward only registers the redirect, no connection yet).
for p in $(seq "$PORT_LO" "$PORT_HI"); do
    adb forward "tcp:$p" "tcp:$p" >/dev/null 2>&1
done

echo "[emulator-smoke] polling 127.0.0.1:{$PORT_LO..$PORT_HI}/console.txt for the boot banner (<= ${BOOT_TIMEOUT_S}s)"
FOUND_PORT=""
CONSOLE=""
DEADLINE=$((SECONDS + BOOT_TIMEOUT_S))
while [ "$SECONDS" -lt "$DEADLINE" ] && [ -z "$FOUND_PORT" ]; do
    for p in $(seq "$PORT_LO" "$PORT_HI"); do
        C="$(curl -s --max-time 1 "http://127.0.0.1:$p/console.txt" 2>/dev/null)"
        if [ -n "$C" ]; then
            FOUND_PORT="$p"; CONSOLE="$C"
            break
        fi
    done
    [ -n "$FOUND_PORT" ] && break
    sleep 2
done

if [ -z "$FOUND_PORT" ]; then
    fail "[boot-port]" "no node answered GET /console.txt on any port $PORT_LO-$PORT_HI within ${BOOT_TIMEOUT_S}s"
    echo "----- adb logcat tail (diagnostics only -- kernel stdout is NOT routed here, see header) -----"
    adb logcat -d -t 200 2>/dev/null | tail -200
    exit 1
fi
echo "[emulator-smoke] node answered on port $FOUND_PORT"

echo "$CONSOLE" | grep -qF "$BOOT_MARKER" \
    || fail "[boot-marker]" "console.txt (port $FOUND_PORT) never printed: $BOOT_MARKER"
echo "$CONSOLE" | grep -qF "$SHELL_PROMPT" \
    || fail "[shell-prompt]" "console.txt (port $FOUND_PORT) never reached the shell prompt"

# --- /galaxy.json structural check ------------------------------------------
GJ="$(curl -s --max-time 5 "http://127.0.0.1:$FOUND_PORT/galaxy.json" 2>/dev/null)"
echo "[emulator-smoke] /galaxy.json: $GJ"
if [ -z "$GJ" ]; then
    fail "[galaxy-json]" "no response from /galaxy.json"
elif have_python; then
    echo "$GJ" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert "me" in d and "id" in d["me"]' 2>/dev/null \
        || fail "[galaxy-json]" "/galaxy.json is not valid JSON with a me.id field"
else
    echo "$GJ" | grep -qE '"me":\{"id":[0-9]+' \
        || fail "[galaxy-json]" "/galaxy.json missing the expected me.id shape (no python3 to parse strictly)"
fi

# --- bonus: /ask round-trip (NOT consent-gated, unlike /teach) -------------
# Informational only -- does not gate the job. /teach requires the ark-profile
# consent handshake (POST /profile with a content-id-bound ack); wiring that
# up is future work (it would also guard the 90feb043-class consent/CSS
# regression on the SAME machinery, but from the data side, not pixels). /ask
# is explicitly NOT consent-gated (galaxy.c comment: "the SHELL mouth is NOT
# gated (operator trust)"), so a well-formed JSON reply -- refusal or a real
# answer -- both prove the HTTP -> mind_cmd -> HTTP path is alive end-to-end.
ASK="$(curl -s --max-time 5 --data 'k=sky' "http://127.0.0.1:$FOUND_PORT/ask" 2>/dev/null)"
echo "[emulator-smoke] /ask k=sky (informational, not gated): $ASK"

adb shell dumpsys battery reset >/dev/null 2>&1 || true

if [ "$DIAG" -ne 0 ]; then
    echo ""
    echo "----- console.txt tail (port $FOUND_PORT) -----"
    echo "$CONSOLE" | tail -80
    echo "----- FAIL: one or more runtime assertions failed, see above -----"
    exit 1
fi

echo ""
echo "[emulator-smoke] PASS -- node on port $FOUND_PORT booted the real kernel thread," \
     "printed the T-Kernel banner, reached the shell prompt, and served /galaxy.json."
exit 0
