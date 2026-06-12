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

[ -f "$MK" ] || { echo "check_parity: missing $MK" >&2; exit 2; }
[ -f "$CM" ] || { echo "check_parity: missing $CM" >&2; exit 2; }

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
    ' "$MK" | tr ' \t' '\n\n' | grep -E '\.(c|S)$' | sort -u
}
# alias: same robust extractor handles inline single/multi-line lists too.
mk_inline() { mk_list "$1"; }
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

if [ "$DRIFT" -ne 0 ]; then
    echo ""
    echo "check_parity: FAIL — Android CMakeLists.txt has drifted from boot/linux/Makefile."
    echo "Fix the source list (or add a documented allowlist entry) and re-run."
    exit 1
fi

echo "check_parity: OK — Android CMakeLists.txt is in lock-step with boot/linux/Makefile."
exit 0
