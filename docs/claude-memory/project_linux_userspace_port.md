---
name: project-linux-userspace-port
description: Planned arch/linux/ port — p-kernel runs as a Linux userspace process so anyone can try it without RPi hardware. Design decisions locked in 2026-05-21.
metadata: 
  node_type: memory
  type: project
  originSessionId: 43b22a68-07a3-4c6e-9e55-cd3fcf682431
---

**Why this exists:** to make p-kernel testable by anyone with a Linux box, no QEMU/no RPi/no JTAG. `git clone && make && ./p-kernel` should produce a working boot banner. This is the distribution-and-adoption multiplier for the project.

**Framing (important):** this is **HAL passthrough**, not syscall passthrough. T-Kernel API stays as-is. We write a new `arch/linux/` that maps hardware abstractions (UART, timer, NIC, IRQ vector, context switch) to Linux equivalents (termios, SIGALRM, TUN/TAP, signal handlers, raw asm switching). UML-style syscall passthrough is explicitly out of scope — too hard, not the goal.

**The central insight:** POSIX signals **are** userspace IRQs. SIGALRM == timer IRQ, SIGIO on stdin == UART RX IRQ, SIGIO on TAP fd == network RX IRQ. The signal handler IS the exception vector. Once you accept this, the rest of the port mirrors the existing arch/aarch64 structure.

## Context-switch decision: raw assembly per host arch (locked 2026-05-21)

Three options were considered:

1. **`ucontext_t` + `swapcontext()`** — rejected. POSIX 2008 deprecated. Hidden `sigprocmask` syscall per switch. Abstraction leak (libc-specific behavior).
2. **1 task = 1 pthread** — rejected. Hands scheduling to Linux, breaks T-Kernel's design intent ("the kernel schedules"). Loses real-time semantics.
3. **Raw assembly per host arch** — **chosen.** Same model as Fiasco-UX. Preserves T-Kernel scheduling. Faster (~20ns vs ucontext's ~200ns). Aligns with the bare-metal `cpu_support.S` already written for x86 and aarch64.

User's stated rationale: deprecation = "no future-proofing on a serious project." Aligns with the project's "this should still run in 10 years" ethos.

## The five components (~110 lines total)

1. **`arch_ctx_t` struct** — 7 callee-saved regs only (rbx/rbp/r12-r15/rsp for x86_64). ~10 lines.
2. **`arch_ctx_switch.S`** — save 7 regs, load 7 regs, `ret`. ~20 lines.
3. **`task_trampoline` + `arch_init_ctx`** — set up initial stack with trampoline as return address; trampoline loads arg from r12 / entry from r13, calls entry, falls through to task_exit. ~25 lines.
4. **Flag-based IRQ mask** — `irq_disabled` + `pending_irqs` sig_atomic_t pair. Signal handler either dispatches or sets pending bit. ~40 lines.
5. **`arch_alloc_stack`** — `mmap(... MAP_STACK)` + `mprotect` guard page. ~15 lines.

## Build/test sequence (planned)

**Session 1: standalone proof-of-concept. DONE 2026-05-21.** Components 1, 2, 5 in a single .c+.S pair. Two dummy tasks that yield to each other via `arch_ctx_switch` and printf alternately. No T-Kernel yet. Validates the assembly + stack layout in isolation.

Files created (all under `arch/linux/`):
- `include/arch_ctx.h` — `arch_ctx_t` for both `__aarch64__` (13 regs, 104 B) and `__x86_64__` (7 regs, 56 B), plus the prototype.
- `aarch64/ctx_switch.S` — 17 instructions / 68 bytes of machine code. Saves x19-x30 + sp into prev, loads same from next, `ret`.
- `aarch64/poc_ctx_switch.c` — driver with `alloc_stack` (mmap + mprotect guard page) and a minimal `init_ctx` that sets `ctx->x30 = entry` and `ctx->sp = aligned stack top`.
- `aarch64/Makefile` — standalone build (will be absorbed by the kernel build in Session 3).

Verified output (aarch64-linux host, Termux proot Ubuntu):
```
[main] host=aarch64-linux, stack_a top=0x7e00d70000, stack_b top=0x7e00d5f000
[main] entering task A
[A] counter=0
[B] counter=1
[A] counter=2
[B] counter=3
[A] counter=4
[B] limit reached, yielding to main
[main] back in main — PoC complete
```

**Why this matters:** the C compiler resumed task_a's loop variable state across two intervening switches into task_b. That is end-to-end proof that callee-saved regs + sp form the complete "where we were" picture for a cooperative dispatch — no need for ucontext's signal-mask save, no need for pthread's parallel execution. The remaining components of the port (trampoline, signals, sio termios, TUN/TAP) all sit on this single foundation.

PoC gotcha already caught and noted in source comments: a minimal `init_ctx` that sets x30 = entry will infinite-loop if entry ever returns (the function's epilogue restores x30 from the stack, where the prologue had saved the value we passed in — i.e., entry itself). Session 2's trampoline fixes this; the PoC sidesteps by making tasks never return.

**Session 2: signal-based preemption + trampoline. DONE 2026-05-21.** Adds preemptive context switching alongside Session 1's cooperative path. Two tight-loop tasks alternated under a 10 ms SIGALRM with no cooperative yields; after 30 ticks both counters were ~30M, near-equal.

Files (`arch/linux/`):
- `include/arch_preempt.h` — `arch_full_ctx_t` (mcontext_t + populated flag), `arch_signals_init`, `arch_timer_start`, `arch_irq_disable/enable`, `arch_init_full_ctx`.
- `aarch64/trampoline.S` — `task_trampoline`: mov x0,x20 / blr x19 / bl task_exit. Lets task entries return without infinite-looping back into themselves.
- `aarch64/poc_preempt.c` — bootstraps via `raise(SIGALRM)`, handler rewrites `uc->uc_mcontext`, sigreturn drops control into the next task. Escapes back to main via `siglongjmp` after `tick_limit` ticks.

Key technique: **never call `arch_ctx_switch` from a signal handler.** Doing so would save the signal stack's sp (because the handler runs on `sigaltstack`), which the next signal would clobber. Instead, the handler modifies the saved `mcontext_t` in the signal frame; the kernel's `sigreturn` restores from the modified frame and execution lands in a different task. `mcontext_t` is **not** the deprecated `swapcontext` API — it is the standard POSIX surface for signal-saved CPU state.

### Lesson captured: aarch64 mcontext `__reserved` must contain a valid FPSIMD section

Initial implementation did `memset(&ctx->mc, 0, sizeof(mcontext_t))` in `arch_init_full_ctx`, then `memcpy(&uc->uc_mcontext, &ctx->mc, ...)` on first install. Result: SIGSEGV exit 139 on first sigreturn.

Root cause: Linux arm64 sigreturn validates `__reserved` via `parse_user_sigframe()` → `restore_fpsimd_context()` and returns `-EINVAL` if no FPSIMD section is found. Zeroing `__reserved` removes the tagged section list (which begins with FPSIMD), so the kernel rejects the sigframe and the failing sigreturn is reported as SIGSEGV.

Fix: factor out three primitives and use them with intent.
- `save_into_ctx(ctx, uc)` — full memcpy uc→ctx. Safe because uc came from the kernel with a well-formed `__reserved`.
- `restore_populated(uc, ctx)` — full memcpy ctx→uc. Safe because ctx was sourced from a previous `save_into_ctx`.
- `install_fresh(uc, ctx)` — **only patch `regs[0..30]`, `sp`, `pc`** into uc, leaving uc's `__reserved` tail (the live kernel-built one) in place. Used for tasks that have never run yet.

The rule: round-trip a captured mcontext freely (memcpy is fine in both directions); when constructing a fresh mcontext from nothing, patch the named fields onto a live one instead of replacing it wholesale.

Save to memory for future arch/linux porting work — this same gotcha will reappear on x86_64-linux (different `__reserved` contents but same validation requirement) and on aarch64-macOS (different OS, same idea: kernel/Mach validates `_STRUCT_MCONTEXT64`).

**Session 3a: T-Kernel dispatcher port. DONE 2026-05-21.** The real cpu_support.S contract — `knl_dispatch`, `knl_dispatch_to_schedtsk`, `knl_task_entry_trampoline`, `knl_timer_handler_startup`, `knl_taskmode` — now compiles and runs in Linux userspace. Verified end-to-end with a unit test (`poc_dispatch.c`) that builds fake TCBs with the real 112-byte dormant frame layout, then drives them through the actual dispatcher.

Files added:
- `arch/linux/aarch64/include/cpu_status.h` — shadow of the AArch64 header. `ENABLE_INTERRUPT`/`DISABLE_INTERRUPT`/`disint`/`enaint` now operate on `arch_irq_disabled_flag` instead of `msr daifset/clr`. CTXB layout unchanged (so `TCB_SSP=192` stays valid).
- `arch/linux/aarch64/include/cpu_insn.h` — sibling shadow. `disint`/`enaint`/`isDI`/`knl_getBASEPRI` inline functions reference the same flag. Other helpers (`knl_define_inthdr`, `knl_isTaskIndependent`, etc.) are byte-identical to the AArch64 sibling.
- `arch/linux/aarch64/cpu_support.S` — port of the AArch64 dispatcher with these surgical changes:
    1. `msr daifset, #0x3` → inline write of 1 to `arch_irq_disabled_flag` via `adrp + add + str`. x16/x17 used as scratch (AAPCS64 designates these for intra-procedure calls — caller-saved, not used for args).
    2. `msr daifclr, #0x3` → inline write of 0 to the same flag. **Does NOT drain pending signals** on the dispatch exit path, to avoid re-entering knl_dispatch from a signal handler. Lazy delivery is acceptable because SIGALRM keeps firing.
    3. `eret` → `ret` (we're in EL0-equivalent userspace).
    4. `wfe` idle → `bl sched_yield`. The signal still arrives; we loop.
    5. EL1 exception vector table (`el1_vectors`, `_vec_*`) → **deleted entirely**. Signals replace it.
- `arch/linux/aarch64/poc_dispatch.c` — drives the dispatcher with hand-rolled fake TCBs. Two tasks alternate via `knl_dispatch`, the second yields back to a `main_tcb` via `knl_dispatch_to_schedtsk`. Verified output:
    ```
    [main] dispatching to task A
    [A] entered with stacd=0xa1
    [A] counter=0
    [B] entered with stacd=0xb2
    [B] counter=1 ... [A] counter=4
    [B] limit reached, switching to main_tcb
    [main] resumed; counter=5, exit clean
    ```

Architectural insight worth keeping: **the T-Kernel dispatcher contract is host-OS-independent.** All it needs from arch is the ability to (a) toggle an "IRQ masked" predicate, (b) push/pop the 112-byte frame on a task-owned stack, (c) jump to a trampoline that reads the TCB and calls the task entry. None of these need privileged instructions. The Linux port reuses 95% of the dispatcher logic verbatim — only the entry/exit IRQ control and the EL1 vectors changed.

Build quirk: T-Kernel's `include/lib/libc` directory contains its own (incomplete) `stdio.h` etc. that shadows the system libc. The dispatcher PoC works around this by NOT adding `include/lib/libc` to its include path — only `cpu_support.S` (which needs `offset.h`) uses the T-Kernel header set. Real Session 3b will need to either fork these libc headers per-arch or change the build to avoid the conflict.

**Session 3b: T-Kernel boots through all module inits. DONE 2026-05-21.** End-to-end skeleton — `./p-kernel` builds (350 KB) and runs all 12 InitModule calls cleanly. Hangs at the next step (knl_Imalloc → tk_cre_tsk_impl) — turned out to be a DI/EI macro shadow + SSP alignment, not the LP64 allocator. Both fixed in 3c below.

Files added under `arch/linux/aarch64/`:
- `cpu_init.c` — 16 MB BSS heap, knl_intvec init, knl_init_Imalloc call.
- `tkdev_init.c` — `knl_tkdev_initialize` wires arch_signals_init + arch_timer_start (100 Hz).
- `sio.c` — termios raw-mode wrapper. **POSIX-only TU** (no T-Kernel headers) to dodge wchar_t/va_list collisions; INT/UB defined locally as int/unsigned char, ABI-compatible.
- `preempt.c` — SIGALRM handler calls knl_timer_handler_startup. arch_irq_disabled_flag + pending-tick drain via arch_irq_enable_with_drain.
- `pci.c`, `rtl8139.c`, `arch_reboot.c`, `vfs_stub.c` — stubs.
- `inittask_def.c`, `inittask_main.c`, `usermain.c` — minimal versions; usermain just prints a banner and loops on tk_dly_tsk.
- `include/tkdev_timer.h` — **critical shadow**; bare-metal sibling uses `msr cntp_*_el0` which traps SIGILL from EL0. Linux version stubs all hooks.

Files added under `boot/linux/`:
- `main.c`, `Makefile`, `.gitignore`.

Header patches (kept for all builds, guarded behind `_TK_HOSTED_LIBC_` etc.):
- `include/typedef.h` — int*_t typedefs gated against system stdint guards.
- `include/stddef.h` — added wchar_t so glibc <stdlib.h> compiles.
- `arch/aarch64/include/stdarg.h` — added __gnuc_va_list for glibc <stdio.h>.
- `arch/linux/aarch64/include/cpu_insn.h` — DSB/ISB inline + `#undef isDI` to override the DAIF macro from syslib_depend.h.

Build quirk learned: **never put `-I include/lib/libc` in the arch/linux include path.** Those placeholder headers (stdio.h is 5 lines, signal.h has no struct sigaction) shadow the system libc and break POSIX-using TUs. Bare-metal builds still need them.

**Session 3c entry point: fix the LP64 allocator.** Once knl_Imalloc returns a valid pointer, tk_cre_tsk_impl should complete and usermain runs.

---

**Session 3c: ./p-kernel runs T-Kernel end-to-end. DONE 2026-05-21.**

Boot banner reaches usermain. Real bugs found by adding traces and
narrowing — NOT what I expected from the Session 3b post-mortem:

**Bug A: DI/EI macro shadow.** `arch/aarch64/include/syslib_depend.h`
defines DI / EI to call `_aa64_disint()` (which executes `msr
daifset, #0x3` — EL1 privileged). That header is reached via
syslib_common.h → `<syslib_depend.h>` and the include path makes
the aarch64 version win in our Linux build too. Every `DI(imask)`
in kernel/common code (Imalloc, task_manage, etc.) was raising
SIGILL silently — a real hang that surfaced first inside the
deepest path (knl_Imalloc).

Fix: shadow with `arch/linux/aarch64/include/syslib_depend.h`
that supplies `_linux_disint`/`_linux_enaint` over the flag-based
arch_irq_disabled_flag + arch_irq_enable_with_drain exports from
preempt.c.

**Bug B: SSP alignment.** `arch/aarch64/include/cpu_task.h`'s
`knl_setup_context` computed `ssp = (UB *)tcb->isstack - 112`.
knl_Imalloc only guarantees 8-byte alignment, so SSP landed at
e.g. 0x4322a8 (mod 16 == 8). After the dispatcher executed
`mov sp, x1` with x1=SSP, the very next `stp` (in a debug push)
faulted because AArch64 requires sp 16-byte aligned for every
sp-relative memory access. The fault surfaced as a clean
process exit because the signal frame's saved PC was nowhere
sane and the SIGSEGV propagated weirdly.

Fix: in `knl_setup_context`, round `tcb->isstack` down to a
16-byte boundary before subtracting DORMANT_STACK_SIZE. Bare-metal
builds get the same alignment guarantee for free (it was a latent
bug there too — the existing AArch64 port escaped only because
the heap addresses it uses happened to be 16-aligned).

**Lesson reusable across future hosted-port work:**
1. **When something hangs but the host's signal-or-exit behaviour
   is murky, suspect privileged-instruction SIGILL inside an
   inline / macro.** Shadow headers (`*_depend.h`) are the
   highest-probability hiding place.
2. **AArch64's "sp must be 16-byte aligned" applies to EL0 too.**
   Any allocator handing memory to the dispatcher's `mov sp, x`
   needs to enforce that, not just 8-byte stack-element alignment.

Verified boot:
```
=== p-kernel linux boot ===
[INIT] termios stdin/stdout
[BOOT] Starting T-Kernel...
[T-Kernel] Initial task started
 p-kernel  [linux / aarch64 userspace]
  T-Kernel is alive inside a Linux process.
```

The dispatcher idles (sched_yield path), SIGALRM ticks at 10 ms,
tk_dly_tsk(1000) re-wakes init_task each second. The full
cooperative + preemptive scheduling loop is alive.

**Session 3d items (no urgency):**
- Wire the time-domain fixes ([[project-linux-userspace-time-domain]]).
- ~~Bring up arch/common (AI primitives + distributed layer).~~ **DONE 2026-05-21.**
- Add an interactive shell on stdin (sio_read_line is ready).
- x86_64-linux sibling port.

---

**Session 3d: arch/common linked, AI banner lit. DONE 2026-05-21.**

All 27 portable sources in `arch/common/` (tensor, ai_job, pipeline, ai_stats, netstack, drpc, swim, kdds, replica, pmesh, sfs, heal, degrade, vital, edf, raft, spawn, moe, dtr, dkva, dmn, ga, fedlearn, mem_store, chat, dproc, kloader_task) now build into the Linux p-kernel binary without modification. The "common" refactor on the bare-metal track paid off — the same .c files compile for hosted.

Only **two small arch hooks** had to land to close the link:

- `arch/linux/aarch64/rtl8139.c` gained `rtl8139_send` (returns -1) and `rtl8139_get_mac` (fabricates 52:54:00:12:34:56, the QEMU virt default — netstack treats this MAC as "single-node mode" and never tries to join a cluster). Phase B will replace these with real network backends.
- `arch/linux/aarch64/cpu_init.c` defines `_kernel_end` via inline asm. kloader_task uses it as a "do-not-overwrite-kernel" sentinel. Bare-metal gets it from the linker script.

`usermain` now mirrors the no-network subset of `arch/aarch64/usermain.c`:

```c
ai_kernel_init();   // tensor pool, ai_job queue, pipeline, MLP
kdds_init();        // pub/sub single-node
dtr_init();         // 568-parameter Distributed Transformer
```

The boot banner is now byte-identical to QEMU virt / RPi 3 modulo the build-id line:

```
 p-kernel  [linux / aarch64 userspace]
[ai]   Tensor pool   : 16 slots × 16 KB
[ai]   AI job queue  : 8 slots (software NPU)
[ai]   Pipeline      : 16 frames zero-copy
[ai]   MLP model     : 4→8→8→3 sensor classifier
[kdds] K-DDS ready  port=7376
[dtr] Transformer initialized
[dtr]   arch  : Embed(4tok×8) + MHSA(h=2,dk=4) + FFN(16) + Cls(3)
[dtr]   params: 568 floats
[dtr]   dist  : SOLO=local / REDUCED=TensorPar / FULL=Pipeline
```

Binary size: 813 KB (up from 420 KB without arch/common). Still tiny by Linux app standards. Suitable for shipping in an APK.

**This completes "Phase A foundation" for UMP** — the substrate that the Android app will wrap. What remains for Phase A proper: PIC build, JNI bridge, Android terminal view in the UI. The kernel itself is ready.

**Remaining Session 3 items:**
- ~~Interactive shell~~ **DONE.** Shell with `ai`, `dtr`, `kdds`, `net`, `rx`, `ver`, `exit` commands. (commit 541781c)
- ~~2-node mesh via local virtual NIC~~ **DONE 2026-05-21** (commit d25b951). UDP loopback ports 29001..29008. SWIM discovery + SUSPECT lifecycle confirmed in cross-process test.
- Wall-clock alignment ([[project-linux-userspace-time-domain]]).
- x86_64-linux sibling.
- LP64 allocator pointer-cast cleanup ([[feedback-lp64-allocator-trap]]) — still latent, not blocking.

---

**Session 3e: 2-node mesh on Linux. DONE 2026-05-21.**

Two `./p-kernel` processes on a single Linux host now find and gossip with each other via the standard SWIM protocol. From the cluster's perspective they're indistinguishable from two bare-metal nodes on a QEMU-virt bridge.

Implementation: a tiny UDP-loopback "virtual wire" in `arch/linux/aarch64/net_unix.c`. Each node binds 127.0.0.1:(29000 + PKERNEL_NODE_ID); `rtl8139_send` sendto's every other slot in 1..8. The whole bridge is ~80 lines of POSIX-only code.

The unblock that made this possible: **`sio_read_line` was blocking the entire Linux process at the syscall level**, so T-Kernel's `net_task` never got scheduled. Fix: `O_NONBLOCK` on stdin + `tk_dly_tsk(10)` between polls. The shell now yields, T-Kernel sees scheduler opportunities, every other task runs. This is a general lesson for the hosted port: **any Linux syscall that blocks the process freezes the entire kernel**; replace blocking calls with polling + `tk_dly_tsk`.

### proot quirks learned the hard way

- **AF_UNIX path-bound sockets**: `stat()` succeeds on the socket file but cross-process `sendto` returns ENOENT. Confirmed with a 12-line C reproducer.
- **AF_UNIX abstract namespace**: `bind` succeeds, `getsockname` returns the right name with `sun_path[0]==0`, but cross-process `sendto` returns ECONNREFUSED. Self-send (same process) works fine. proot is intercepting the address-namespace lookup in a way that diverges between the binder's view and the sender's view.
- **UDP on 127.0.0.1**: works flawlessly through proot. This is the substrate we use.

On a native Android NDK build (Phase A), neither proot nor its quirks apply — TUN/TAP or AF_UNIX abstract may become viable again. UDP loopback works there too.

### Cluster-level proof captured in the commit log

```
node 1: [swim] node 1 discovered  (via rx)
node 2: [swim] node 0 discovered  (via rx)
        [swim] node 0 -> SUSPECT (no response)
        [swim] node 0 recovered  (via rx)
```

The full bare-metal SWIM lifecycle (discovery → suspect → recovery) runs on Linux.

### ~~Caveat — single-node mode currently more stable than 2-node~~ — RESOLVED 2026-05-21 + re-verified 2026-05-22

Earlier note recorded "under heavy bidirectional traffic with both SWIM tasks alive, one node occasionally segfaults." Triaged in commit `2f07ce6` (2026-05-21) — root cause was the LP64 allocator pointer-truncation bug fixed in `dc04a52`, which corrupted the freelist on any heap address above 4 GiB and produced cryptic later faults. The defensive `dnode_table` zero-init added in 2f07ce6 is documentation-in-code more than a fix.

Re-verified 2026-05-22 (after the lp64 refactor): 10 × 30-second symmetric soaks, plus late-joining-node, plus 5 rapid restarts of node 2 while node 1 stayed up — **zero abnormal exits**, full SWIM lifecycle observed (discover → SUSPECT → recovered → SOLO/REDUCED degrade transitions) on every run. The 2-node mesh is solid.

**Session 4+: TUN/TAP + distributed layer.** Two `./p-kernel` instances on the same host talk DRPC/SWIM/Raft over virtual NICs. AArch64 host port follows the same pattern in `arch/linux/aarch64/`.

## Gotchas captured in advance

- **Signal during context switch**: solve with `sigaltstack()` — register a dedicated stack for signal handlers so they don't corrupt mid-switch task stacks.
- **FP/SIMD save**: not needed at context_switch (xmm0-15 are caller-saved per System V x86_64 ABI). Tensor ops will work without explicit save.
- **gdb unwinding through switches**: add `.cfi_def_cfa_offset` directives to `arch_ctx_switch.S` so DWARF describes the stack pointer dance correctly.
- **System V vs Windows x64 ABI**: target Linux only. Windows hosts use WSL.

## Out of scope (for now)

- Windows native, macOS native (use WSL / OrbStack respectively).
- Multi-core simulation (single Linux thread for the whole p-kernel kernel; SMP is a much later project).
- Full POSIX syscall passthrough for usermain tasks (T-Kernel apps stay T-Kernel apps; no fopen-on-host-filesystem hack).

## Relation to other work

- Does not block Phase 3 (RPi 3 hardware) — orthogonal track. Either can progress while the other waits.
- Once linux port works, **CI for arch/common can run in a few seconds** without QEMU. Big quality-of-life win for [[project-aarch64-next-steps]].
- ASan/UBSan/Valgrind become available on arch/common code — surfaces bugs invisible on bare metal.
