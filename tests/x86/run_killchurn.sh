#!/bin/sh
# tests/x86/run_killchurn.sh
#
# KILL-CHURN-CRASH regression harness (gap-ledger: VENDOR-PATCH-LOSS).
#
# WHY THIS EXISTS
# ---------------
# On 2026-07-02 the μT-Kernel 3.0 migration (f50c30a0) silently DELETED the
# kill/churn hardening, and the known bug KILL-CHURN-CRASH was live again for
# 41 days without a single red light.  339a66a2 restored the hardening.  The
# ROOT CAUSE of the 41 days was not the deletion — it was that NOTHING in CI
# could tell a hardened tree from an unhardened one.  This script is that
# missing detector.  It is deliberately tree-agnostic: point it at ANY
# checkout and it will tell you whether that tree reproduces the disease.
#
# WHAT IT DOES
# ------------
# Clean-builds boot/x86 and boots it under QEMU N times.  Each boot is fed the
# SAME five verbs the CI `ring3 生存 + マインドゲート` job feeds
# (.github/workflows/ci.yml, job `ring3-survival`):
#
#     ring3 iso  ->  ring3 test  ->  ring3 mind  ->  dproc test  ->  fpu test
#
# marker-gated exactly as CI does it (each verb sent only after the previous
# verb's gate tag appears on the serial log).  That five-verb feed is the ONLY
# known-effective reproducer: it churns ring-3 tasks through TCB slots, and the
# crash lands on a slot REUSE, right after `[elf] task started (tid=NN)`.
#
# NOT USED, ON PURPOSE: the `dproc churn` shell verb (arch/x86/shell.c:1888).
# At wave-45 it was measured to reproduce NOTHING on master — a blank round.
# Wiring a blank round into CI is worse than no gate at all, so it stays out.
#
# N: THE DEFAULT IS 40, THE GATE USES 150 — AND 40 IS NOT ENOUGH
# --------------------------------------------------------------
# Original estimate: signature A at 8.0 %/boot on an UNFIXED tree (18 hits in
# 225 audited boots), 0/225 on the fixed tree, so P(40 boots all miss) =
# 0.92^40 ~= 3.6 %.  The 2026-08-12 Stage-2 run (710 boots, both trees,
# interleaved, one host, one day) measured the rate DIRECTLY:
#     unfixed 74e23947 : 23/355 (6.48 %)   fixed 468a1c67 : 0/355
#     Fisher exact p = 1.65e-07; every hit at the same EIP 0x0015382E.
# Sizing uses the IDLE arm (18/280 = 6.43 %/boot, Wilson 95 % CI [4.10 %,
# 9.93 %]) because that is the regime a serialized CI runner is in, and it is
# sized against the LOWER bound 4.10 %/boot:
#     N=40 misses a live regression 18.7 % of the time — one green in five is
#     a lie.  N=100 -> 1.5 %.  N=150 -> 0.19 %.
# Cost is 2.86–2.91 s/boot including the clean build, so N=150 is ~7.8 min.
# The CI job (.github/workflows/ci.yml, `killchurn-x86`) therefore runs with
# KILLCHURN_N=150.  The default below is still 40 for interactive/smoke use;
# if you are deciding whether a tree is hardened, pass 150 or more.  N is a
# POWER decision, not a runtime knob.
#
# THE TWO SIGNATURES (get this wrong and the gate is worthless)
# ------------------------------------------------------------
# Signature A — THE THING THIS GATE GUARDS.  Fails the run.
#     Error Code: 0x00000002   (supervisor WRITE to a not-present page)
#     CS=0x00000008            (ring-0: the kernel faulted, not a tenant)
#     EIP inside knl_wait_release_tmout
#   That function's body is an inlined QueRemove; the faulting store is the
#   `entry->prev->next = entry->next` write (`mov %eax,(%edx)`).  We classify
#   on the SYMBOL RANGE, never on a hardcoded `+0x11`: the byte offset of that
#   store moves with codegen (it is +0x17 on the current fixed tree, it was
#   +0x11 on the unfixed tree measured on 2026-08-12).  The range is stable;
#   the offset is not.
#
# Signature B — a DIFFERENT, unsolved bug.  Counted and reported, never fatal.
#     EIP is a WILD PC outside the kernel's .text (observed: 0x00000000,
#     0x0000001F, 0xF000E2C3), with Error Code 0x00000000.
#   Registered in docs/architecture/gap-ledger.md as OPEN row KCC-WILDPC.  It
#   fires at ~7.6 % per boot and — this is the decisive measurement — that rate
#   did NOT move across the 339a66a2 fix (p = 1.00).  It is not our bug.  If we
#   reddened on it, CI would be red ~96 % of the time at N=40, and the only
#   thing the team would learn is to ignore red.  So: COUNT IT, PRINT IT, EXIT 0.
#
# HOW THE SIGNATURES ARE RESOLVED (a trap that already bit us once)
# ----------------------------------------------------------------
# EIP is resolved against boot/x86/bootloader.bin — NOT kernel.elf.  Both are
# ELFs built from the same objects, but through DIFFERENT linker scripts:
# linker.ld puts *(.text.start) FIRST, kernel.ld puts it LAST, so every .text
# symbol is shifted by 0x1b4 between them.  QEMU is booted from bootloader.bin
# (`-kernel bootloader.bin`), so bootloader.bin is the only correct symbol
# source.  Resolving against kernel.elf lands mid-instruction and attributes
# the fault to the wrong function — that is exactly the mistake made on
# 2026-08-12 and caught by audit.
#
# CR2 IS NOT USED.  boot/x86/idt.c:266 prints CR2 only under -DKCC_DIAG, which
# the default build does not set.  Classification is mechanical, from the
# error-code bits and EIP alone.
#
# EXIT CODE
# ---------
#   0  no signature A (even if signature B / INCOMPLETE boots occurred)
#   1  signature A seen at least once  <- the regression is back
#   2  harness could not run (build failed, no symbols, no QEMU)
#
# The machine-readable verdict line, always printed, always greppable:
#   [killchurn] sigA=0 sigB=3 clean=37 incomplete=0 N=40 other=0
#
# ENVIRONMENT
#   KILLCHURN_N            boots to run                     (default 40)
#   KILLCHURN_BOOT_TIMEOUT hard wall-clock cap per boot, s  (default 120)
#   KILLCHURN_SKIP_BUILD   1 = reuse the existing build     (default 0)
#   KILLCHURN_KEEP         1 = keep the scratch dir + logs  (default 0)
#   KILLCHURN_T_EXC_DRAIN  cap on draining an exception dump, s (default 2)
#   CROSS / QEMU / NM / OBJDUMP  toolchain overrides
#
# WHAT THIS HARNESS WAS ACTUALLY MEASURED TO DO (2026-08-12, this runner)
#   Signature B is the only live fault available on a FIXED tree, so it is the
#   only way to check that this harness still lands in the regime the audit
#   measured.  With the shipped script, on the fixed tree:
#     2 sequential N=40 runs, host idle    : signature B   2 /  80  (2.5 %)
#     3 concurrent N=25 runs (loaded host) : signature B   8 /  75  (10.7 %)
#     pooled                               : signature B  10 / 155  (6.5 %)
#   Pooled 6.5 % sits right on the audited 7.6 %, so the harness IS in the
#   audited regime.  But idle-vs-loaded is 2.5 % vs 10.7 % (Fisher p ~ 0.05):
#   SUGGESTIVE, not proven, that host load raises the rate.  If it is real,
#   an idle CI runner has less power than the N=40 arithmetic assumes.  The
#   loaded regime is reproduced by launching J copies of this script in
#   parallel: the BOOT phase is concurrency-safe (each invocation boots a
#   private copy of disk.img — see "disk image isolation" below), the BUILD
#   phase is NOT, so parallel copies must build once and then run with
#   KILLCHURN_SKIP_BUILD=1.  That is exactly how 8/75 was measured.
#   HISTORICAL NOTE, so nobody re-derives it: an earlier draft let all boots
#   share the repo's disk.img, and saw signature B 0/120 idle.  Whether the
#   per-boot pristine copy fixed that or 0/120 was luck is NOT established
#   (0/120 vs 2/80 is Fisher p ~ 0.17).  Either way, sharing the image is
#   also a re-entrancy and idempotency bug, which is why it is gone.
#   ANSWERED BY STAGE 2 (2026-08-12): signature A does NOT track load the way
#   signature B appears to.  Unfixed tree, idle 6.43 % vs 3-way concurrent
#   6.67 % (Fisher p = 1.00).  No special load condition is needed to reproduce
#   it, and an idle CI runner is not a weaker detector for signature A.
#
# HONESTY
#   The RED path is no longer hypothetical.  Stage 2 (2026-08-12, 710 boots,
#   interleaved arms, one host, one day) ran this harness against the UNFIXED
#   tree 74e23947 and the fixed tree 468a1c67: 23/355 vs 0/355 signature A,
#   5/5 runs exit 1 vs 5/5 runs exit 0, Fisher p = 1.65e-07.  So this script is
#   now wired as a BLOCKING gate (.github/workflows/ci.yml, `killchurn-x86`).
#   What that does NOT mean:
#     - A green run does not prove the tree is hardened.  It proves that N
#       boots did not hit an event measured at ~6.5 %/boot on an unfixed tree.
#       At N=150 that miss is ~0.19 %; at the default N=40 it is ~18.7 %.
#     - The 2026-07-02 vendor-patch loss removed NINE hardening classes.
#       Signature A pins ONE of them.  A future vendor-tree swap that deletes a
#       DIFFERENT one is not guaranteed to be caught here.
#     - Every rate above was measured on ONE host ([self-hosted,
#       pkernel-thinkpad]) on ONE day.  Another host's per-boot rate, and
#       therefore its required N, is unmeasured.
#   This is a REGRESSION ALARM with a measured false-negative rate, not a proof
#   of correctness.
#
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BOOT="$ROOT/boot/x86"

N="${KILLCHURN_N:-40}"
BOOT_TIMEOUT="${KILLCHURN_BOOT_TIMEOUT:-120}"
SKIP_BUILD="${KILLCHURN_SKIP_BUILD:-0}"
KEEP="${KILLCHURN_KEEP:-0}"

# Per-step marker ceilings (seconds).  These are BACKSTOPS, not budgets: a
# healthy boot walks the whole feed in ~10 s.  Every wait is additionally
# clamped by the per-boot deadline below, so the ceilings can be generous
# without letting one wedged boot eat the run.
T_PROMPT="${KILLCHURN_T_PROMPT:-90}"
T_STEP="${KILLCHURN_T_STEP:-60}"

# Ceiling on draining a ring-0 exception dump before QEMU is killed — see "the
# dump-truncation race" in run_one().  Also a BACKSTOP, not a budget: what it
# waits for is ~40 bytes already queued on an emulated serial port, so the
# normal cost is a single 0.05 s poll.  The ceiling only ever gets spent by a
# dump that will NEVER become classifiable (e.g. exception 3, which idt.c
# answers with "Breakpoint exception handled" and no CS/EIP line at all).
T_EXC_DRAIN="${KILLCHURN_T_EXC_DRAIN:-2}"

QEMU="${QEMU:-qemu-system-x86_64}"
# Mirror boot/x86/Makefile's cross-toolchain autodetection so the symbols we
# read come from the same binutils that linked the image.
if [ -z "${CROSS:-}" ]; then
    if command -v i686-linux-gnu-gcc >/dev/null 2>&1; then
        CROSS="i686-linux-gnu-"
    else
        CROSS=""
    fi
fi
NM="${NM:-${CROSS}nm}"
OBJDUMP="${OBJDUMP:-${CROSS}objdump}"

QPID=""
SCRATCH=""

cleanup() {
    # Requirement: stop QEMU by EXPLICIT PID or via this trap.  NEVER pkill /
    # killall — this repository's path contains "p-kernel", and a pattern kill
    # would take down unrelated processes (including the live services on this
    # host).  $QPID is the `timeout` wrapper; TERM is forwarded to QEMU.
    if [ -n "$QPID" ]; then
        kill "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
        QPID=""
    fi
    if [ -n "$SCRATCH" ] && [ "$KEEP" != "1" ]; then
        rm -rf "$SCRATCH"
    fi
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

die() { echo "[killchurn] FATAL: $*" >&2; exit 2; }

command -v "$QEMU" >/dev/null 2>&1 || die "$QEMU not found"
command -v "$NM"   >/dev/null 2>&1 || die "$NM not found"

SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/killchurn.XXXXXX")"

# ---------------------------------------------------------------- build ----
if [ "$SKIP_BUILD" = "1" ]; then
    echo "[killchurn] KILLCHURN_SKIP_BUILD=1 — reusing the existing $BOOT build"
else
    echo "[killchurn] clean build: $BOOT (all disk)"
    ( cd "$BOOT" && make clean ) >"$SCRATCH/build.log" 2>&1 || true
    if ! ( cd "$BOOT" && make all disk ) >>"$SCRATCH/build.log" 2>&1; then
        tail -60 "$SCRATCH/build.log" >&2
        die "build failed (full log: $SCRATCH/build.log)"
    fi
fi
[ -f "$BOOT/bootloader.bin" ] || die "$BOOT/bootloader.bin missing after build"
[ -f "$BOOT/disk.img" ]       || die "$BOOT/disk.img missing after build"

# ----------------------------------------------------- symbol resolution ----
# ALWAYS bootloader.bin.  See the header note about the 0x1b4 skew vs
# kernel.elf.  bootloader.bin is a fully linked ELF (the Makefile never
# objcopy's it to flat binary), so nm/objdump read it directly.
SYM_LINE="$("$NM" -S "$BOOT/bootloader.bin" 2>/dev/null \
            | awk '$4 == "knl_wait_release_tmout" { print $1, $2; exit }')" || true
[ -n "$SYM_LINE" ] || die "knl_wait_release_tmout not found in $BOOT/bootloader.bin \
(stripped build, or the symbol was renamed — classification would be vacuous)"
SIGA_LO=$(( 0x$(echo "$SYM_LINE" | cut -d' ' -f1) ))
SIGA_SZ=$(( 0x$(echo "$SYM_LINE" | cut -d' ' -f2) ))
[ "$SIGA_SZ" -gt 0 ] || die "knl_wait_release_tmout has zero size — cannot bound signature A"
SIGA_HI=$(( SIGA_LO + SIGA_SZ ))

# .text extent, used to decide "wild PC" (signature B) vs "a real kernel PC".
TEXT_LINE="$("$OBJDUMP" -h "$BOOT/bootloader.bin" 2>/dev/null \
             | awk '$2 == ".text" { print $4, $3; exit }')" || true
[ -n "$TEXT_LINE" ] || die "cannot read .text header from $BOOT/bootloader.bin"
TEXT_LO=$(( 0x$(echo "$TEXT_LINE" | cut -d' ' -f1) ))
TEXT_HI=$(( TEXT_LO + 0x$(echo "$TEXT_LINE" | cut -d' ' -f2) ))

printf '[killchurn] symbols from bootloader.bin (NOT kernel.elf; 0x1b4 skew)\n'
printf '[killchurn]   knl_wait_release_tmout = [0x%08x, 0x%08x)  (signature A window)\n' \
       "$SIGA_LO" "$SIGA_HI"
printf '[killchurn]   .text                  = [0x%08x, 0x%08x)  (outside => wild PC = signature B)\n' \
       "$TEXT_LO" "$TEXT_HI"
echo "[killchurn] N=$N boots, per-boot cap ${BOOT_TIMEOUT}s, scratch $SCRATCH"
echo

# ---------------------------------------------------- disk image isolation --
# QEMU opens the -drive READ-WRITE and takes an exclusive file lock on it.
# Two consequences, both measured, both fixed by booting a private copy:
#   1) NOT RE-ENTRANT.  A second invocation against the same tree gets
#      `Failed to get "write" lock`, QEMU exits instantly, and the harness
#      reports INCOMPLETE for a reason that has nothing to do with the kernel.
#   2) NOT IDEMPOTENT.  Guest writes land in the repo's build artifact, so
#      boot 40 does not start from the same disk as boot 1.
# We re-copy the pristine image before EVERY boot, so each boot is an
# independent sample — which is the whole premise of the rate arithmetic.
# The QEMU command line is otherwise byte-for-byte the ci.yml one.
PRISTINE="$SCRATCH/disk.pristine.img"
BOOTDISK="$SCRATCH/disk.img"
cp "$BOOT/disk.img" "$PRISTINE"

# ------------------------------------------------------------ the feed ----
DEADLINE=0

# wait_for <tag> <ceiling-s> <log>
#   0 = tag appeared
#   1 = ceiling or per-boot deadline hit
#   2 = the kernel took a ring-0 exception (stop feeding; it is dead)
wait_for() {
    _tag="$1"; _max="$2"; _log="$3"
    _end=$(( $(date +%s) + _max ))
    if [ "$_end" -gt "$DEADLINE" ]; then _end="$DEADLINE"; fi
    while :; do
        if grep -aqF "$_tag" "$_log"; then return 0; fi
        if grep -aqF '=== KERNEL EXCEPTION ===' "$_log"; then return 2; fi
        if [ "$(date +%s)" -ge "$_end" ]; then return 1; fi
        sleep 0.2
    done
}

send() { printf '%s\n' "$1" >&3; }

# run_one <serial-log-path>
run_one() {
    _log="$1"
    _fifo="$SCRATCH/feed.fifo"
    rm -f "$_fifo"
    mkfifo "$_fifo"
    : > "$_log"
    cp "$PRISTINE" "$BOOTDISK"          # every boot starts from the same disk
    DEADLINE=$(( $(date +%s) + BOOT_TIMEOUT ))

    # Same QEMU invocation as the ci.yml ring3-survival job (only the -drive
    # path differs: our private per-boot copy).  `timeout` is the hard backstop
    # (ring3 is known to wedge its idle loop after the last gate); $QPID is the
    # explicit handle the trap kills.
    ( cd "$BOOT" && exec timeout -k 5 "$BOOT_TIMEOUT" "$QEMU" -m 256 \
        -kernel bootloader.bin \
        -serial stdio -display none -cpu qemu64 \
        -netdev user,id=n0 -device rtl8139,netdev=n0 \
        -drive file="$BOOTDISK",format=raw,if=ide,index=0 ) \
        < "$_fifo" > "$_log" 2>&1 &
    QPID=$!

    # Hold the write end open for the whole session so QEMU's serial-in never
    # sees EOF between verbs (same trick as ci.yml).
    exec 3> "$_fifo"

    FEED_STAGE=0
    if wait_for 'p-kernel>'        "$T_PROMPT" "$_log"; then FEED_STAGE=1; send 'ring3 iso';  fi
    if [ "$FEED_STAGE" = 1 ] && wait_for '[iso-userptr]'    "$T_STEP" "$_log"; then FEED_STAGE=2; send 'ring3 test'; fi
    if [ "$FEED_STAGE" = 2 ] && wait_for '[ring3-survival]' "$T_STEP" "$_log"; then FEED_STAGE=3; send 'ring3 mind'; fi
    if [ "$FEED_STAGE" = 3 ] && wait_for '[ring3-mind]'     "$T_STEP" "$_log"; then FEED_STAGE=4; send 'dproc test'; fi
    if [ "$FEED_STAGE" = 4 ] && wait_for '[dproc-teardown]' "$T_STEP" "$_log"; then FEED_STAGE=5; send 'fpu test';   fi
    if [ "$FEED_STAGE" = 5 ] && wait_for '[fpu-ctx]'        "$T_STEP" "$_log"; then FEED_STAGE=6; fi

    # ------------------------------------------- the dump-truncation race ----
    # wait_for returns 2 the INSTANT it sees '=== KERNEL EXCEPTION ==='.  But
    # idt.c prints the dump in three pieces and the piece classify() needs is
    # the LAST one (boot/x86/idt.c, the ring-0 arm of the handler):
    #     === KERNEL EXCEPTION ===  /  Exception: <name>
    #     Error Code: 0xNNNNNNNN          (only for 8, 10-14, 17)
    #     CS=0xNNNNNNNN EIP=0xNNNNNNNN    <-- classification lives or dies here
    # Killing QEMU on the banner can cut the capture off mid-"Error Code:".
    # classify() then finds no EIP and files the boot as `other` — and `other`
    # does not set the exit code, so A TRUNCATED SIGNATURE A IS A GREEN GATE.
    # Observed once in 780 boots (2026-08-13); made deterministic, and shown
    # to flip sigA=1/exit 1 into other=1/exit 0, with a stub QEMU that stalls
    # between the banner and the CS/EIP line.
    #
    # So: once a dump has STARTED, wait for it to become classifiable before
    # killing.  This is paid ONLY by boots that actually faulted (~7-11 %/boot
    # here, all signature B on a fixed tree) and it ends the moment the line
    # lands: MEASURED at 6 ms on each real signature-B boot, i.e. satisfied by
    # the first poll, so N=150 costs ~0.1 s.  The regex demands
    # all EIGHT hex digits print_hex32() emits: a half-written 'EIP=0x0015'
    # must not be mistaken for a complete one, or the fix reintroduces the bug
    # it is here to remove.
    if grep -aqF '=== KERNEL EXCEPTION ===' "$_log"; then
        _drain_end=$(( $(date +%s) + T_EXC_DRAIN ))
        while ! grep -aqE 'EIP=0x[0-9A-Fa-f]{8}' "$_log"; do
            if [ "$(date +%s)" -ge "$_drain_end" ]; then break; fi
            sleep 0.05
        done
    fi

    exec 3>&-
    kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
    QPID=""
    rm -f "$_fifo"
}

# ------------------------------------------------------- classification ----
# Sets: VERDICT, plus EXC_ERR / EXC_CS / EXC_EIP when an exception was seen.
classify() {
    _log="$1"
    EXC_ERR=""; EXC_CS=""; EXC_EIP=""

    if ! grep -aqF '=== KERNEL EXCEPTION ===' "$_log"; then
        # No ring-0 fault.  Did the whole five-verb feed actually land?
        if [ "$FEED_STAGE" = 6 ]; then
            VERDICT="clean"
        else
            VERDICT="incomplete"
        fi
        return 0
    fi

    # idt.c prints 8 uppercase hex digits via print_hex32().  The "Error Code:"
    # line only exists for exceptions that push one (8, 10-14, 17); when it is
    # absent we say so explicitly rather than letting it read as a zero code —
    # "no error code at all" and "error code 0" are different faults.
    EXC_ERR="$(grep -a -m1 -o 'Error Code: 0x[0-9A-Fa-f]*' "$_log" | cut -d' ' -f3 || true)"
    if [ -z "$EXC_ERR" ]; then EXC_ERR="<none>"; fi
    EXC_CS="$(grep -a -m1 -o 'CS=0x[0-9A-Fa-f]*'  "$_log" | cut -d'=' -f2 || true)"
    EXC_EIP="$(grep -a -m1 -o 'EIP=0x[0-9A-Fa-f]*' "$_log" | cut -d'=' -f2 || true)"

    if [ -z "$EXC_EIP" ]; then
        EXC_CS="${EXC_CS:-<none>}"; EXC_EIP="<none>"
        VERDICT="other"
        return 0
    fi
    _eip=$(( EXC_EIP ))

    # Signature A: supervisor write to a not-present page, from ring 0, with
    # the PC inside knl_wait_release_tmout's inlined QueRemove.
    if [ "$EXC_ERR" = "0x00000002" ] && [ "$EXC_CS" = "0x00000008" ] \
       && [ "$_eip" -ge "$SIGA_LO" ] && [ "$_eip" -lt "$SIGA_HI" ]; then
        VERDICT="sigA"
        return 0
    fi

    # Signature B (KCC-WILDPC): the PC is not in .text at all — execution went
    # somewhere that holds no kernel code.  A different, unsolved bug.
    if [ "$_eip" -lt "$TEXT_LO" ] || [ "$_eip" -ge "$TEXT_HI" ]; then
        VERDICT="sigB"
        return 0
    fi

    # A ring-0 fault that is neither known signature.  Reported loudly, but it
    # does not set the exit code: the gate's contract is signature A only, and
    # a brand-new unknown must not silently become "the KILL-CHURN regression".
    VERDICT="other"
}

# dump_exception <log> — the EXCEPTION block plus the churn context before it
# (the `[elf] task started (tid=NN)` line is the TCB-slot reuse we care about).
dump_exception() {
    _log="$1"
    _ln="$(grep -an '=== KERNEL EXCEPTION ===' "$_log" | head -1 | cut -d: -f1)"
    if [ -z "$_ln" ]; then return 0; fi
    _from=$(( _ln - 25 ))
    if [ "$_from" -lt 1 ]; then _from=1; fi
    _to=$(( _ln + 14 ))
    echo "----- serial log lines ${_from}..${_to} -----"
    sed -n "${_from},${_to}p" "$_log"
    echo "---------------------------------------------"
}

# ------------------------------------------------------------- the run ----
n_sigA=0; n_sigB=0; n_clean=0; n_incomplete=0; n_other=0
i=1
while [ "$i" -le "$N" ]; do
    LOG="$SCRATCH/boot$(printf '%03d' "$i").log"
    _t0=$(date +%s)
    run_one "$LOG"
    classify "$LOG"
    _dt=$(( $(date +%s) - _t0 ))

    case "$VERDICT" in
        sigA)
            n_sigA=$(( n_sigA + 1 ))
            echo "[killchurn] boot $i/$N: *** SIGNATURE A *** err=$EXC_ERR CS=$EXC_CS EIP=$EXC_EIP  (${_dt}s)"
            echo "[killchurn]   EIP is inside knl_wait_release_tmout [$(printf '0x%08x' "$SIGA_LO"), $(printf '0x%08x' "$SIGA_HI"))"
            echo "[killchurn]   => KILL-CHURN-CRASH reproduced: the kill/churn hardening is MISSING in this tree."
            dump_exception "$LOG"
            ;;
        sigB)
            n_sigB=$(( n_sigB + 1 ))
            echo "[killchurn] boot $i/$N: signature B (KCC-WILDPC, wild PC, NOT this gate's bug) err=$EXC_ERR CS=$EXC_CS EIP=$EXC_EIP  (${_dt}s)"
            ;;
        other)
            n_other=$(( n_other + 1 ))
            echo "[killchurn] boot $i/$N: UNCLASSIFIED ring-0 exception err=$EXC_ERR CS=$EXC_CS EIP=$EXC_EIP  (${_dt}s)"
            echo "[killchurn]   neither signature A nor B — not fatal here, but somebody should look."
            dump_exception "$LOG"
            ;;
        incomplete)
            n_incomplete=$(( n_incomplete + 1 ))
            echo "[killchurn] boot $i/$N: INCOMPLETE — feed stalled at stage $FEED_STAGE/6 (no exception; QEMU/host slowness or a wedge)  (${_dt}s)"
            # Show the tail: a stall at stage 0 is usually QEMU itself refusing
            # to start (image lock, missing accel), not the kernel at all.
            echo "----- last 5 lines of the serial capture -----"
            tail -5 "$LOG"
            echo "----------------------------------------------"
            ;;
        clean)
            n_clean=$(( n_clean + 1 ))
            echo "[killchurn] boot $i/$N: clean (all five gates)  (${_dt}s)"
            ;;
    esac

    # Every boot's serial log is kept for the whole run (~15 KB each), so a
    # post-mortem can re-examine ANY boot, not just the ones we flagged.  The
    # EXIT trap removes the scratch dir at the end unless KILLCHURN_KEEP=1.
    echo "$VERDICT $LOG" >> "$SCRATCH/verdicts.txt"
    i=$(( i + 1 ))
done

echo
echo "[killchurn] sigA=$n_sigA sigB=$n_sigB clean=$n_clean incomplete=$n_incomplete N=$N other=$n_other"
if [ "$KEEP" = "1" ]; then
    echo "[killchurn] KILLCHURN_KEEP=1 — serial logs left in $SCRATCH"
fi

if [ "$n_incomplete" -gt 0 ]; then
    echo "[killchurn] NOTE: $n_incomplete boot(s) never finished the five-verb feed."
    echo "[killchurn]       Those boots did NOT exercise the churn path, so the effective"
    echo "[killchurn]       sample is $(( N - n_incomplete )), not $N. Re-run if this is large."
fi
if [ "$n_sigB" -gt 0 ]; then
    echo "[killchurn] NOTE: signature B (gap-ledger OPEN: KCC-WILDPC) fired $n_sigB/$N time(s)."
    echo "[killchurn]       Expected ~7.6%/boot; measured unchanged across the 339a66a2 fix."
    echo "[killchurn]       It is a separate unsolved bug and does NOT fail this gate."
fi

if [ "$n_sigA" -gt 0 ]; then
    echo "[killchurn] FAIL: KILL-CHURN-CRASH (signature A) reproduced $n_sigA/$N."
    echo "[killchurn]       gap-ledger: VENDOR-PATCH-LOSS. The kill/churn hardening"
    echo "[killchurn]       (restored by 339a66a2) is absent or broken in this tree."
    exit 1
fi

echo "[killchurn] PASS: signature A not seen in $N boots."
echo "[killchurn]       Stage 2 measured the unfixed rate at 6.43%/boot (18/280 idle-serial,"
echo "[killchurn]       Wilson 95% CI 4.10-9.93%); at the CI lower bound an unfixed tree slips"
echo "[killchurn]       through a clean N=$N run with probability ~$(awk "BEGIN{printf \"%.2f\", 100*(0.9590^$N)}")%."
echo "[killchurn]       Green is evidence, not proof. Signature A does NOT track host load"
echo "[killchurn]       (idle 6.43% vs 3-parallel 6.67%, Fisher p=1.00), so an idle runner is"
echo "[killchurn]       not weaker — an EARLIER note claiming otherwise was refuted by measurement."
exit 0
