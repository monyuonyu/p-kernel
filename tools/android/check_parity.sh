#!/bin/sh
# tools/android/check_parity.sh
#
# Drift guard for the Android APK build (wave-36 lesson: the CMake source
# list silently fell ~10 waves behind boot/linux/Makefile, so galaxy/R3/
# living-mind/signing organs weren't even compiled into the shipped .so).
#
# This script extracts the per-section source lists from BOTH:
#   - boot/linux/Makefile            (the canonical hosted module list)
#   - android/app/src/main/cpp/CMakeLists.txt
# and diffs them. It exits NONZERO on any drift so CI can fail the build.
#
# Sections compared (by basename, the only thing that matters for "is this
# TU in the build"): arch/common (COMMON), arch/linux shared (ARCH_SHARED),
# arch/linux/aarch64 per-arch (ARCH), kernel/common (KERNEL), libc string.
#
# If a file is LEGITIMATELY host-only or android-only, add its basename to
# the matching ALLOW_* list below WITH A COMMENT explaining why. An empty
# allowlist is the healthy state: the two builds should ship the same TUs.
#
# Usage:  tools/android/check_parity.sh            (from repo root or anywhere)
#         exit 0 = in lock-step ; exit 1 = drift (prints the offending files)

set -eu

# --- locate the repo root (this script lives at tools/android/) -----------
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

MK="$ROOT/boot/linux/Makefile"
CM="$ROOT/android/app/src/main/cpp/CMakeLists.txt"
# MK2: the x86_64 host Makefile. boot/linux/Makefile is the aarch64 host build;
# boot/linux_x86_64/Makefile is the x86_64 host build. The AI/common + shared
# module source lists MUST be identical between the two — that is exactly the
# modver-drift hole that only the real x86_64 host caught (a module added to one
# host Makefile but not the other silently ships a different mind per arch).
MK2="$ROOT/boot/linux_x86_64/Makefile"

[ -f "$MK" ]  || { echo "check_parity: missing $MK" >&2; exit 2; }
[ -f "$CM" ]  || { echo "check_parity: missing $CM" >&2; exit 2; }
[ -f "$MK2" ] || { echo "check_parity: missing $MK2" >&2; exit 2; }

# --- allowlists: basenames that may legitimately appear in only one side ---
# Keep these EMPTY unless there is a real Bionic/host reason; document each.
#
# (none today — the host and Android builds ship the identical TU set. selfc
#  is in BOTH lists; on Android it compiles as a stub because HAVE_LIBTCC is
#  never defined for the NDK build, so no fork() under Bionic/SELinux. That
#  is a compile-time guard inside selfc.c, NOT a source-list difference, so
#  selfc.c does NOT belong in an allowlist.)
ALLOW_MK_ONLY=""      # basenames the Makefile may have that CMake omits
ALLOW_CM_ONLY=""      # basenames CMake may have that the Makefile omits

# --- extractor ------------------------------------------------------------
# Makefile: pull a `NAME = ...` assignment, INCLUDING any backslash
# continuation lines, whether the content starts on the `=` line or not.
# Splits on whitespace, keeps only *.c / *.S basenames.
mk_list() {
    # $1 = variable name; optional $2 = Makefile path (default $MK).
    _mkf="${2:-$MK}"
    awk -v var="$1" '
        # Start: the assignment line for this variable.
        $0 ~ "^"var" *=" {
            sub("^"var" *= *","")
            cont = (/\\$/)
            sub(/\\$/,"")
            print
            f = cont
            next
        }
        # Continuation lines while we are inside the block.
        f {
            cont = (/\\$/)
            sub(/\\$/,"")
            print
            f = cont
        }
    ' "$_mkf" | tr ' \t' '\n\n' | grep -E '\.(c|S)$' | sort -u
}
# alias: same robust extractor handles inline single/multi-line lists too.
mk_inline() { mk_list "$1" "${2:-$MK}"; }
# CMake: pull a `set(NAME ... )` block, take the basename of each path.
cm_list() {
    awk -v var="$1" '
        $0 ~ "^set\\("var"($| )" {f=1; next}
        f && /^\)/ {f=0}
        f { print }
    ' "$CM" | sed -E 's@.*/@@; s@[ )]@@g' | grep -E '\.(c|S)$' | sort -u
}

# --- compare one section --------------------------------------------------
DRIFT=0
compare() {
    section="$1"; mk_file="$2"; cm_file="$3"; allow_mk="$4"; allow_cm="$5"

    only_mk=$(comm -23 "$mk_file" "$cm_file")
    only_cm=$(comm -13 "$mk_file" "$cm_file")

    for f in $only_mk; do
        case " $allow_mk " in *" $f "*) continue;; esac
        echo "DRIFT [$section] in Makefile but MISSING from CMake: $f"
        DRIFT=1
    done
    for f in $only_cm; do
        case " $allow_cm " in *" $f "*) continue;; esac
        echo "DRIFT [$section] in CMake but MISSING from Makefile: $f"
        DRIFT=1
    done
}

# --- compare one section between the two HOST Makefiles -------------------
# Same drift logic as compare(), but both sides are Makefile variables (one
# from boot/linux/Makefile, one from boot/linux_x86_64/Makefile). The shared
# AI/common + kernel + libc lists are arch-INDEPENDENT and MUST match exactly.
compare_hosts() {
    section="$1"; a_file="$2"; b_file="$3"

    only_a=$(comm -23 "$a_file" "$b_file")
    only_b=$(comm -13 "$a_file" "$b_file")

    for f in $only_a; do
        echo "DRIFT [$section] in boot/linux/Makefile but MISSING from boot/linux_x86_64/Makefile: $f"
        DRIFT=1
    done
    for f in $only_b; do
        echo "DRIFT [$section] in boot/linux_x86_64/Makefile but MISSING from boot/linux/Makefile: $f"
        DRIFT=1
    done
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# COMMON (arch/common AI + distributed layer)
mk_list COMMON_C_SRCS         > "$TMP/mk_common"
cm_list COMMON_SRC            > "$TMP/cm_common"
compare COMMON "$TMP/mk_common" "$TMP/cm_common" "$ALLOW_MK_ONLY" "$ALLOW_CM_ONLY"

# ARCH_SHARED (arch/linux cross-arch hosted)
mk_inline ARCH_SHARED_C_SRCS  > "$TMP/mk_shared"
cm_list ARCH_SHARED_SRC       > "$TMP/cm_shared"
compare ARCH_SHARED "$TMP/mk_shared" "$TMP/cm_shared" "$ALLOW_MK_ONLY" "$ALLOW_CM_ONLY"

# ARCH (arch/linux/aarch64 per-arch C + S)
{ mk_list ARCH_C_SRCS; mk_inline ARCH_S_SRCS; } | sort -u > "$TMP/mk_arch"
cm_list ARCH_SRC              > "$TMP/cm_arch"
compare ARCH "$TMP/mk_arch" "$TMP/cm_arch" "$ALLOW_MK_ONLY" "$ALLOW_CM_ONLY"

# KERNEL (kernel/common)
mk_list KERNEL_SRCS           > "$TMP/mk_kern"
cm_list KCOMMON_SRC           > "$TMP/cm_kern"
compare KERNEL "$TMP/mk_kern" "$TMP/cm_kern" "$ALLOW_MK_ONLY" "$ALLOW_CM_ONLY"

# LIBSTR (lib/libc/string)
mk_inline LIBSTR_SRCS         > "$TMP/mk_libstr"
cm_list LIBSTR_SRC            > "$TMP/cm_libstr"
compare LIBSTR "$TMP/mk_libstr" "$TMP/cm_libstr" "$ALLOW_MK_ONLY" "$ALLOW_CM_ONLY"

# RELAY (relay/sha256.c) — single file, just confirm both have it.
mk_inline RELAY_C_SRCS        > "$TMP/mk_relay"
cm_list RELAY_SRC             > "$TMP/cm_relay"
compare RELAY "$TMP/mk_relay" "$TMP/cm_relay" "$ALLOW_MK_ONLY" "$ALLOW_CM_ONLY"

# --- host-vs-host parity: boot/linux (aarch64) vs boot/linux_x86_64 -------
# The arch-INDEPENDENT source lists must be byte-identical between the two host
# builds. COMMON_C_SRCS is the AI/common (arch/common) module list — the mind
# itself — and is the load-bearing one (the modver-drift hole the real x86_64
# host caught: a module added to one host but not the other). KERNEL/LIBSTR/
# RELAY/ARCH_SHARED are likewise arch-independent. ARCH_C_SRCS/ARCH_S_SRCS are
# DELIBERATELY per-arch (arch/linux/aarch64 vs arch/linux/x86_64) and so are NOT
# parity-checked here; their basenames happening to match is incidental.
mk_list COMMON_C_SRCS       "$MK"  > "$TMP/h_common_a"
mk_list COMMON_C_SRCS       "$MK2" > "$TMP/h_common_b"
compare_hosts COMMON-HOST "$TMP/h_common_a" "$TMP/h_common_b"

mk_inline ARCH_SHARED_C_SRCS "$MK"  > "$TMP/h_shared_a"
mk_inline ARCH_SHARED_C_SRCS "$MK2" > "$TMP/h_shared_b"
compare_hosts ARCH_SHARED-HOST "$TMP/h_shared_a" "$TMP/h_shared_b"

mk_list KERNEL_SRCS         "$MK"  > "$TMP/h_kern_a"
mk_list KERNEL_SRCS         "$MK2" > "$TMP/h_kern_b"
compare_hosts KERNEL-HOST "$TMP/h_kern_a" "$TMP/h_kern_b"

mk_inline LIBSTR_SRCS       "$MK"  > "$TMP/h_libstr_a"
mk_inline LIBSTR_SRCS       "$MK2" > "$TMP/h_libstr_b"
compare_hosts LIBSTR-HOST "$TMP/h_libstr_a" "$TMP/h_libstr_b"

mk_inline RELAY_C_SRCS      "$MK"  > "$TMP/h_relay_a"
mk_inline RELAY_C_SRCS      "$MK2" > "$TMP/h_relay_b"
compare_hosts RELAY-HOST "$TMP/h_relay_a" "$TMP/h_relay_b"

if [ "$DRIFT" -ne 0 ]; then
    echo ""
    echo "check_parity: FAIL — a source list has drifted (Android CMake vs boot/linux/Makefile,"
    echo "or boot/linux/Makefile vs boot/linux_x86_64/Makefile)."
    echo "Fix the source list (or add a documented allowlist entry) and re-run."
    exit 1
fi

echo "check_parity: OK — Android CMakeLists.txt and both host Makefiles are in lock-step."
exit 0
