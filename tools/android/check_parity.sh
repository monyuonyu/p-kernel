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
# arch/linux/aarch64 per-arch (ARCH), kernel/mtkernel3 (KERNEL).
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
# (none today for COMMON/ARCH/SHARED/KERNEL/RELAY — the host and Android builds
#  ship the identical TU set. selfc is in BOTH lists; on Android it compiles as a
#  stub because HAVE_LIBTCC is never defined for the NDK build, so no fork() under
#  Bionic/SELinux. That is a compile-time guard inside selfc.c, NOT a source-list
#  difference, so selfc.c does NOT belong in an allowlist.)
#
# --- 2026-07-12 (wave-android-buildfix): the eight files that used to live here
#     (ss6_live.c supernode.c net_relay_tcp.c supernode_autopromote.c
#      compat_arkfs_gap.c compat_ota.c conscience.c gen_succession.c) were the
#     ACCUMULATED COMMON/ARCH drift — declared "host-only FOR NOW" while no
#     Android SDK was around to verify the NDK compile. They are now MIRRORED into
#     the CMake source lists and the APK links + builds with them (verified: NDK
#     26.3, gradlew assembleDebug BUILD SUCCESSFUL, app-debug.apk produced). So the
#     allowlist is back to EMPTY — the healthy state. conscience.c now links on
#     Android too (r3_incontext.c references conscience_check unconditionally; the
#     cradle ingest path's CRADLE_HAS_CONSCIENCE gate stays host-only, a
#     compile-time flag, NOT a source-list difference).
ALLOW_MK_ONLY=""     # basenames the Makefile may have that CMake omits
ALLOW_CM_ONLY=""     # basenames CMake may have that the Makefile omits

# --- LLM tier allowlists (arch/common/llm) --------------------------------
# The host builds the FULL SmolLM2 teacher+student engine (LLM_C_SRCS +
# DMOE_OBJS). The Android .so builds the SAME set with ONE documented swap and
# two Android-only additions:
#   frontier.c        : host-only. Android links frontier_stub.c instead — weak
#                       no-op Frontier-Mouth fallbacks (design frontier_mouth
#                       §1.3/§4: "Android-bionic nodes without the companion
#                       simply link the stub and are byte-honest baseline nodes
#                       forever"). So frontier.c is MK-only, frontier_stub.c is
#                       CM-only.
#   student_stub.c    : Android-only weak fallbacks for the student ABI. On the
#                       host the strong student_shell.c wins; on Android both are
#                       linked and the strong defs still win (weak loses), so it
#                       is harmless belt-and-suspenders. CM-only.
ALLOW_LLM_MK_ONLY="frontier.c"                    # host builds full frontier.c; APK stubs it
ALLOW_LLM_CM_ONLY="student_stub.c frontier_stub.c" # Android-only weak fallbacks

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

# KERNEL (kernel/mtkernel3 — μT-Kernel 3.0 コア + 起動/libtm/sysdepend)
# Makefile 側はコア(MTK3_KNL_SRCS)・sysdepend(MTK3_SYSDEP_SRCS)・
# 固定名オブジェクト(MTK3_MISC_OBJS 相当)の 3 群に分かれるため合算する。
{ mk_list MTK3_KNL_SRCS; mk_list MTK3_SYSDEP_SRCS; \
  printf 'sysinit.c\ninittask.c\nstring.c\nbitop.c\nlibtm.c\nlibtm_printf.c\ntm_com.c\ndispatch.S\n'; } \
  | sort -u > "$TMP/mk_kern"
cm_list MTK3_KNL_SRC          > "$TMP/cm_kern"
compare KERNEL "$TMP/mk_kern" "$TMP/cm_kern" "$ALLOW_MK_ONLY" "$ALLOW_CM_ONLY"

# LIBSTR は 2.0 廃止と同時に削除（hosted は Bionic/glibc の string を使用）

# RELAY (relay/sha256.c) — single file, just confirm both have it.
mk_inline RELAY_C_SRCS        > "$TMP/mk_relay"
cm_list RELAY_SRC             > "$TMP/cm_relay"
compare RELAY "$TMP/mk_relay" "$TMP/cm_relay" "$ALLOW_MK_ONLY" "$ALLOW_CM_ONLY"

# LLM (arch/common/llm — the SmolLM2 teacher+student engine).
# THIS SECTION IS THE BLIND SPOT THE wave-36 REWRITE NEVER ADDED: the LLM (and,
# via ARCH, the net) source lists were NOT compared, so student_shell.c's
# teacher-GGUF probe (gguf/forward/dev_capacity), the SS-6 live transport, and
# usermain's llm_shell_cmd drifted out of the CMake lists and were only caught at
# NDK --no-undefined link time. Now the guard catches them first.
#
#   Host  = LLM_C_SRCS + DMOE_OBJS (dmoe_bank.o — a SEPARATE Makefile var, so it
#           is appended by hand; mk_list only sees *.c/*.S tokens).
#   CMake = LLM_STUDENT_SRC + LLM_TEACHER_SRC + LLM_STUB_SRC (the same TUs, split
#           across three set() lists by compile-flag tier).
# Allowlist swap: host frontier.c <-> Android frontier_stub.c (+ student_stub.c).
{ mk_list LLM_C_SRCS; printf 'dmoe_bank.c\n'; } | sort -u > "$TMP/mk_llm"
{ cm_list LLM_STUDENT_SRC; cm_list LLM_TEACHER_SRC; cm_list LLM_STUB_SRC; } \
    | sort -u > "$TMP/cm_llm"
compare LLM "$TMP/mk_llm" "$TMP/cm_llm" "$ALLOW_LLM_MK_ONLY" "$ALLOW_LLM_CM_ONLY"

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

mk_list MTK3_KNL_SRCS       "$MK"  > "$TMP/h_kern_a"
mk_list MTK3_KNL_SRCS       "$MK2" > "$TMP/h_kern_b"
compare_hosts KERNEL-HOST "$TMP/h_kern_a" "$TMP/h_kern_b"

mk_inline RELAY_C_SRCS      "$MK"  > "$TMP/h_relay_a"
mk_inline RELAY_C_SRCS      "$MK2" > "$TMP/h_relay_b"
compare_hosts RELAY-HOST "$TMP/h_relay_a" "$TMP/h_relay_b"

# LLM_C_SRCS is the arch-independent SmolLM2 engine list; it must be byte-
# identical between the two host builds (same drift class as COMMON-HOST — a TU
# added to one host's LLM tier but not the other ships a different mind per arch).
mk_list LLM_C_SRCS          "$MK"  > "$TMP/h_llm_a"
mk_list LLM_C_SRCS          "$MK2" > "$TMP/h_llm_b"
compare_hosts LLM-HOST "$TMP/h_llm_a" "$TMP/h_llm_b"

if [ "$DRIFT" -ne 0 ]; then
    echo ""
    echo "check_parity: FAIL — a source list has drifted (Android CMake vs boot/linux/Makefile,"
    echo "or boot/linux/Makefile vs boot/linux_x86_64/Makefile)."
    echo "Fix the source list (or add a documented allowlist entry) and re-run."
    exit 1
fi

echo "check_parity: OK — Android CMakeLists.txt and both host Makefiles are in lock-step."
exit 0
