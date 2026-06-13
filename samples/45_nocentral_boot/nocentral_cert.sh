#!/bin/bash
# ===========================================================================
# 45_nocentral_boot / nocentral_cert.sh  —  NOCENTRAL-RAFT (wave-55).
#
# The external audit (PR #7) found a thesis contradiction: the project's
# manifesto is "no central anything", but the BARE-METAL builds used to
# elect a privileged Raft LEADER at boot (a central coordinator) via:
#
#   arch/x86/usermain.c       raft_init(); create_task(raft_task, ...)
#   arch/aarch64/usermain.c   raft_init(); try_task(raft_task, ...)
#
# The linux builds already did NOT spawn raft at boot — they run the
# decentralized swim/world stack. Wave-55 makes bare metal match: raft is
# NO LONGER a boot service on any build. Raft stays linkable and is demoted
# to an OPT-IN demo invokable from the shell `raft` verb (lazy init + spawn).
#
# This cert is a BUILD-TIME / SOURCE assertion (the commander accepted a
# grep-based gate when a runtime cert is awkward — the bare-metal boot is a
# qemu-serial flow, not a self-test harness). It asserts:
#
#   (a) NO raft leader task is spawned in the bare-metal BOOT path
#       (neither usermain's boot-init function spawns raft_task).
#   (b) The decentralized stack (swim / world) IS wired at boot on bare
#       metal (the replacement for the central coordinator).
#   (c) The `raft` capability is still reachable on-demand (the shell verb
#       lazily calls raft_init() — the capability is demoted, not deleted).
#   (d) raft.c / raft_task / raft_init still exist (not deleted).
#
# Usage:  ./nocentral_cert.sh
# Prints: [nocentral-boot] PASS  (exit 0)  or  ... FAIL  (exit 1)
# ===========================================================================
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

X86="$ROOT/arch/x86/usermain.c"
AA64="$ROOT/arch/aarch64/usermain.c"
SHELL_X86="$ROOT/arch/x86/shell.c"
RAFT="$ROOT/arch/common/raft.c"

fail() { echo "[nocentral-boot] FAIL — $1"; echo "[nocentral-boot] FAIL"; exit 1; }

# ---------------------------------------------------------------------------
# (a) No raft_task spawn in the bare-metal BOOT path.
#
#     The boot wiring used the literal token `raft_task` next to a
#     create_task/try_task call. Both bare-metal usermains also now host an
#     ON-DEMAND `raft` verb (lazy init+spawn) that legitimately mentions
#     raft_task — so we must scope the check to the BOOT-INIT function, not
#     the whole file.
#
#       x86      — boot wiring is inside usermain()'s init block, between the
#                  "Phase 10" distributed bring-up and the shell start. The
#                  on-demand verb lives in a SEPARATE file (arch/x86/shell.c),
#                  so any raft_task spawn in arch/x86/usermain.c is a regress.
#       aarch64  — boot wiring was inside distributed_init(); the on-demand
#                  verb is inside usermain(). Scope to distributed_init().
# ---------------------------------------------------------------------------
[ -f "$X86" ]  || fail "missing $X86"
[ -f "$AA64" ] || fail "missing $AA64"

# x86: whole-file check is correct (verb is in shell.c, not here). Anchor to
# statement position so the explanatory comment that mentions raft_task /
# raft_init() does not trip the gate.
if grep -nE '^[[:space:]]*(create_task|try_task)[^;]*raft_task' "$X86" >/dev/null; then
    echo "  offending lines in $X86:"; grep -nE '^[[:space:]]*(create_task|try_task)[^;]*raft_task' "$X86"
    fail "x86 boot path still spawns raft_task"
fi
if grep -nE '^[[:space:]]*raft_init[[:space:]]*\(' "$X86" >/dev/null; then
    echo "  offending lines in $X86:"; grep -nE '^[[:space:]]*raft_init[[:space:]]*\(' "$X86"
    fail "x86 boot path still calls raft_init()"
fi

# aarch64: extract the distributed_init() body (the boot-init function) and
# assert it spawns/inits no raft. awk from the function signature to the line
# that closes it at column 0 ('}').
AA64_BOOT="$(awk '/^static void distributed_init\(/{c=1} c{print} c&&/^}/{exit}' "$AA64")"
[ -n "$AA64_BOOT" ] || fail "could not isolate aarch64 distributed_init() body"
if printf '%s\n' "$AA64_BOOT" | grep -E '^[[:space:]]*(create_task|try_task)[^;]*raft_task' >/dev/null; then
    echo "  offending lines in aarch64 distributed_init():"
    printf '%s\n' "$AA64_BOOT" | grep -nE '^[[:space:]]*(create_task|try_task)[^;]*raft_task'
    fail "aarch64 boot path (distributed_init) still spawns raft_task"
fi
# Match a raft_init() CALL statement (first non-space token), not a mention
# inside an explanatory comment (comment bodies start with '*' or '//').
if printf '%s\n' "$AA64_BOOT" | grep -E '^[[:space:]]*raft_init[[:space:]]*\(' >/dev/null; then
    echo "  offending lines in aarch64 distributed_init():"
    printf '%s\n' "$AA64_BOOT" | grep -nE '^[[:space:]]*raft_init[[:space:]]*\('
    fail "aarch64 boot path (distributed_init) still calls raft_init()"
fi
echo "  (a) OK — no raft leader spawned/inited in the boot path (x86 + aarch64)"

# ---------------------------------------------------------------------------
# (b) The decentralized stack IS up at boot on bare metal.
#     x86 boots the world task; aarch64 boots swim + world.
# ---------------------------------------------------------------------------
grep -qE 'world_task' "$X86"  || fail "x86 boot path lost the world task"
grep -qE 'swim_task'  "$AA64" || fail "aarch64 boot path lost the swim task"
grep -qE 'world_task' "$AA64" || fail "aarch64 boot path lost the world task"
echo "  (b) OK — decentralized swim/world stack still wired at boot"

# ---------------------------------------------------------------------------
# (c) The `raft` capability is still reachable on-demand.
#     The x86 shell verb must lazily init raft (capability demoted, not gone).
# ---------------------------------------------------------------------------
grep -qE 'cmd_raft' "$SHELL_X86" || fail "x86 shell lost the raft verb"
grep -qE 'raft_init' "$SHELL_X86" || fail "x86 raft verb no longer lazily inits raft"
# aarch64 usermain hosts its own on-demand raft verb.
grep -qE 'raft_init' "$AA64" || fail "aarch64 raft verb no longer lazily inits raft"
echo "  (c) OK — raft is opt-in on-demand (lazy init in the shell verb)"

# ---------------------------------------------------------------------------
# (d) The raft implementation still exists (not deleted).
# ---------------------------------------------------------------------------
[ -f "$RAFT" ]                         || fail "arch/common/raft.c was deleted"
grep -qE 'void raft_task' "$RAFT"      || fail "raft_task implementation gone"
grep -qE 'void raft_init' "$RAFT"      || fail "raft_init implementation gone"
echo "  (d) OK — raft.c / raft_task / raft_init still linkable"

echo "[nocentral-boot] PASS"
exit 0
