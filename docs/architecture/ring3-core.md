# ring3-core — moving the self-modifying AI core down to ring3/EL0

> Status: **SHIPPED for the inference path (Waves A+B+C); training modules still design** —
> 2026-06-19 doc-status fix. The "written before implementation" line below is STALE.
> What actually shipped: Wave A (design), **Wave B survival** (a ring3 core crash is reaped
> via the SYS_EXIT unwind — `boot/x86/idt.c` saved-CS branch, `user_fault_reap` in
> `arch/x86/syscall.c`, `ring3 test`, the user ELFs under `samples/12_ring3/`; wave-25), and
> **Wave C the mind's math in ring3** (`samples/12_ring3/03_core_mind/core_mind.c` dual-compiles
> `moe.c`+`dtr.c`; weights via 0x213, visibility via 0x214; `arch/x86/elf_loader.c` ring3-core
> path; wave-27). Genuine remainder: dtr training / lm / dmn / gl modules into ring3, async
> 0x240/0x241, x87 FXSAVE before concurrent minds (`arch/x86/fpu.c` debt note), aarch64 EL0 mirror.
> Builds ON: the ring-3 scaffolding that already boots Linux/musl ELFs on
> bare-metal x86 (`arch/x86/userspace.c`, `arch/x86/gdt_user.c`,
> `arch/x86/elf_loader.c`, `arch/x86/syscall.c`, `boot/x86/isr.S`) and the
> AI math in `arch/common/` (`moe.c`, `lm_self.c`, `lm_consolidate.c`, `dtr.c`,
> `reflex.c`, `dmn.c`, `r3_incontext.c`).
> Directive (mk_pino, 2026-06-09): *"ちょっと大変ですけど ring3に降ろしましょう。
> arm は el0 ですかね。やっぱりこの部分は常に変化していく領域なので不安定になると思うので
> ユーザー空間においた方がいいですよね。"*

This document is two parts, like [living-mind.md](living-mind.md):

- **Part I — the north star.** Where the privilege line is cut, the syscall ABI
  the ring-3 core uses to reach back into the kernel, and the genuine forks that
  are the commander's to decide.
- **Part II — the first slice.** ONE concrete, falsifiable proof-of-principle:
  `moe_infer` running in a ring-3 user task on bare-metal x86, **plus** a
  kernel-survives-core-crash proof with an unambiguous acceptance gate.

---

## Part I — the north star

### I.0 Why move the core down

The 5-layer worldview (Body / Brain / Self / Collective / **Evolution**) names the
AI core as the part that *constantly changes itself*. Today that core runs at the
same privilege as the kernel that keeps the node alive:

- x86 bare-metal: AI math executes as **ring-0 kernel tasks** (`moe_task` created
  in `arch/x86/usermain.c:281`; `dmn_task` at `arch/x86/usermain.c:167`;
  `lm_test` / `lm_self_test` / `dtr_infer` invoked directly from the shell at
  `arch/x86/shell.c:494,505,608`).
- aarch64 bare-metal: **everything runs at EL1** — there is *no* EL0/SVC
  scaffolding in `arch/aarch64/` or `boot/aarch64/` at all (verified: the only
  `EL1`/`svc` hits are IRQ-vector comments in `rtl8139.c` / `tkdev_init.c`).

A self-modifying core that lives at ring-0/EL1 can take the kernel down with it.
The proof is already in the tree: the x86 exception handler at
`boot/x86/idt.c:141-143` ends every fault (UD/GP/PF/…) in

```c
while (1) { asm volatile ("hlt"); }     /* boot/x86/idt.c:141 */
```

so a single bad pointer or bad opcode *inside the core* halts the whole node
forever. That is the opposite of p-kernel's north star ("never dies"). Moving the
core to ring-3/EL0 turns "the mind crashed → the node is dead" into "the mind
crashed → the kernel reaps the user task and can restart the mind." The kernel
becomes a **minimal immutable substrate**; the mind becomes a restartable mutable
tenant.

### I.1 The privilege CUT

The rule for the cut: **anything whose corruption must NOT be able to wedge the
node stays in ring-0; anything that is "constantly changing / inherently unstable"
(the directive's 常に変化していく領域) moves to ring-3.** Concretely:

**STAYS in ring-0 (the immutable substrate):**

| Subsystem | Why it stays | Evidence in tree |
|---|---|---|
| Scheduler / dispatcher | Must keep running tasks even when a tenant faults | `kernel/`, `tk_cre_tsk`/`tk_sta_tsk` via `syscall.c` |
| Memory allocator (Imalloc / mpl / mpf) | Substrate must own physical memory & page tables | `boot/x86/memory.c`, `arch/x86/paging.c` |
| Page-table / GDT / TSS / IDT | Privilege machinery itself | `arch/x86/gdt_user.c`, `boot/x86/idt.c`, `arch/x86/userspace.c` |
| p-fs / VFS / block dev | Durable state must outlive a crashed mind | `arch/common/pfs_*.c`, `arch/x86/vfs.c`, `arch/common/arkfs.c` |
| Net stack (UDP/TCP, relay, SWIM, gossip transport) | The Collective's *transport* survives a local-mind crash | `arch/common/netstack.c`, `swim.c`, `net_ssy.c` |
| Syscall dispatch (`int 0x80`) + the AI syscall *thunks* | The boundary itself; the thin wrappers that call the moved math | `arch/x86/syscall.c:242`, `boot/x86/isr.S:313` |
| Tensor store + AI job pool kernel side | Shared substrate the core reads/writes through handles | `arch/common/tensor.c`, `arch/common/ai_job.c` |

**MOVES to ring-3 (the mutable mind):**

| Module | File | Current entry |
|---|---|---|
| MoE gate/route/return | `arch/common/moe.c` | `moe_infer(B,B,B,B)` (`moe.h:164`) |
| Transformer inference | `arch/common/dtr.c` | `dtr_infer(const B[4])` (`dtr.h:153`) |
| Reflex layer | `arch/common/reflex.c` | called from `moe_infer` |
| In-context recall (R3) | `arch/common/r3_incontext.c` | — |
| Self layer (autobiography) | `arch/common/lm_self.c` | `lm_self_test()` (`lm_self.h:84`) |
| Living-mind consolidation | `arch/common/lm_consolidate.c` | `lm_test()` |
| DMN replay / imagination | `arch/common/dmn.c` | `dmn_task` |
| Gossip-learning weight merge (gl) | `arch/common/gossip_learn.c` | `gl_merge` |

The *math* moves; the *handles to durable/shared resources* stay in ring-0 and are
reached only through syscalls (I.2). The core can corrupt its own weights, its own
stack, its own heap — none of which is the kernel's.

#### Where exactly the line gets blurry — COMMANDER DECISION NEEDED

These are the genuine forks. Each has a recommended default and the tradeoff; the
commander picks, the implementer does not silently choose.

> **CDN-1 — DMN sleep-consolidation timer access.**
> `dmn_task` (`arch/x86/usermain.c:167`) currently runs as a ring-0 cyclic/idle
> task and reads the kernel clock + reallocates the engram ring directly.
> In ring-3 it loses direct timer/clock access and direct engram-ring memory.
> **Options:** (a) keep DMN *scheduling* in ring-0 (a tiny ring-0 "wake the mind
> on idle" stub) but run the actual replay/distill math in ring-3 via a syscall;
> (b) move DMN fully to ring-3 and give it `SYS_TK_GET_TIM` + a `SYS_TK_DLY_TSK`
> sleep loop. **Recommended default: (a)** — keep the *when* in ring-0 (it is
> scheduling, part of the substrate) and move the *what* (replay+G22 distill) to
> ring-3. Tradeoff: one extra syscall round-trip per consolidation cycle; DMN's
> timing precision is now bounded by syscall latency (fine — it is a slow,
> rest-time loop, `MOE_BROADCAST_MS`-scale).

> **CDN-2 — gl weight-merge memory model.**
> `gl_merge` (gossip-learning) and `dtr_train` mutate model weights. If weights
> live in ring-0 tensor store, every merge is a syscall write; if weights live in
> the ring-3 core's own heap, the merge is a plain memcpy but the weights die with
> a core crash (the mind forgets on every fault). **Recommended default:**
> weights are **owned by ring-0 p-fs/tensor store** (durable, survive a crash) and
> the ring-3 core operates on a *mapped working copy*, flushing via
> `SYS_WRITE`/tensor syscalls at consolidation boundaries (matches living-mind's
> "memory-grounded, weight-change-last" rung in `living-mind.md I.2`). Tradeoff:
> the core cannot do fine-grained per-step weight writes cheaply; it must batch.
> This is acceptable and arguably *desirable* (it forces the safe batched path).

> **CDN-3 — Collective transport ownership.**
> `moe`/`dtr` publish scores and partial tensors over K-DDS / SWIM / the relay.
> Does the ring-3 core call `SYS_TOPIC_PUB`/`SYS_UDP_SEND` per message (transport
> stays ring-0), or does a thin ring-0 "mesh agent" own the topics and the core
> just hands it buffers? **Recommended default:** transport stays ring-0; the core
> uses the *existing* `SYS_TOPIC_*` (`0x220-0x223`) and `SYS_UDP_*`
> (`0x200-0x208`) syscalls already in `syscall.c:1056` / `:941`. No new mesh
> agent. Tradeoff: more syscalls; but it reuses a proven path and keeps the
> Collective's transport alive when the local mind crashes.

> **CDN-4 — how many ring-3 address spaces.**
> Today `elf_loader.c` supports effectively ONE user task at a time
> (`_uarg` is a single static, `arch/x86/elf_loader.c:82`; comment says "single
> user task (no re-entrancy)"). The core is several cooperating modules. **Options:**
> (a) one ring-3 address space hosting the whole core (simplest; a fault kills the
> whole mind, kernel survives — still satisfies the directive); (b) one ring-3
> space per module (a faulting `dtr` does not kill `moe`). **Recommended default:
> (a)** for the first wave — one mind, one address space — and defer (b) to a later
> wave. Tradeoff: coarse fault isolation now; finer isolation deferred. This keeps
> the first slice small and honest.

### I.2 The syscall ABI back into the kernel

The ring-3 core reaches every kernel service through the existing `int 0x80`
trap gate. Calling convention is already fixed by `boot/x86/isr.S:313-380` and
`syscall_dispatch(W nr, W arg0, W arg1, W arg2)` (`arch/x86/syscall.c:242`):

```
EAX = syscall number (nr)
EBX = arg0
ECX = arg1
EDX = arg2
int 0x80
→ return value in EAX
```

Structs larger than three words are passed by **user-space pointer** in an arg
(the kernel dereferences it under the user's page tables, exactly as
`SYS_TOPIC_SUB`/`SYS_TK_CRE_MTX` already do, e.g. `PK_TOPIC_SUB *` at
`syscall.c:1072`, `PK_CRE_MTX *` at `:1120`). The IDT gate is installed with
DPL=3 so ring-3 may issue it (`syscall.c:208`, flags `0xEF`; `isr.S:288`).

Every kernel service the core needs, mapped to a syscall number. **Bold = NEW,
to be added; the rest already exist and are reused verbatim.**

| Service the core needs | Syscall | Number | Status |
|---|---|---|---|
| Read durable bytes / weights (p-fs) | `SYS_READ` | `3` | exists |
| Write durable bytes / weights (p-fs) | `SYS_WRITE` | `4` | exists |
| Open / close model files | `SYS_OPEN`/`SYS_CLOSE` | `5`/`6` | exists |
| Seek within weight file | `SYS_LSEEK` | `7` | exists |
| Net send/recv (Collective) | `SYS_UDP_SEND`/`RECV` | `0x201`/`0x202` | exists |
| K-DDS publish/subscribe (scores, partials) | `SYS_TOPIC_PUB`/`SUB` | `0x221`/`0x222` | exists |
| Clock / time (DMN cycle, SLA) | `SYS_TK_GET_TIM` | `0x180` | exists |
| Sleep / delay (DMN rest loop) | `SYS_TK_DLY_TSK` | `0x181` | exists |
| Allocate working memory | `SYS_TK_GET_MPL` | `0x162` | exists |
| Free working memory | `SYS_TK_REL_MPL` | `0x163` | exists |
| Inter-task wait/signal (job done) | `SYS_TK_WAI_SEM`/`SIG_SEM` | `0x113`/`0x112` | exists |
| Tensor handle submit/wait (shared substrate) | `SYS_AI_SUBMIT`/`WAIT` | `0x211`/`0x212` | exists |
| **Synchronous local infer entry (bootstrap shim)** | `SYS_INFER` | `0x210` | exists — reused by the slice |
| **SLA-bounded infer** | `SYS_INFER_SLA` | `0x230` | exists (`syscall.c:1085`, `edf_infer`) |

#### Folding in the README:411 SUBMIT/WAIT split — COMMANDER DECISION NEEDED

`boot/x86/README.md:411` has the unchecked TODO:

```
[ ] p_syscall 拡張 (0x230 SYS_INFER_SUBMIT, 0x231 SYS_INFER_WAIT)
    → ring-3 から dtr.c のパイプラインを呼ぶ
```

There is a **collision** the implementer must NOT paper over: `0x230` is *already
defined and live* as `SYS_INFER_SLA` (`p_syscall.h:323`, dispatched at
`syscall.c:1085`). The README wrote `0x230 SYS_INFER_SUBMIT` before `SYS_INFER_SLA`
took that slot. `0x240`/`0x241` `SYS_DTR_SUBMIT`/`SYS_DTR_WAIT` *also* already
exist (`p_syscall.h:328-329`) but `SYS_DTR_SUBMIT` is **synchronous** today
(`syscall.c:1094` calls `dtr_infer` and blocks; `SYS_DTR_WAIT` returns `-1`,
`:1109`).

> **CDN-5 — async infer numbering.** The async SUBMIT/WAIT split the README wants
> is real and needed (a ring-3 core that blocks the whole task on a long
> distributed infer is bad). **Recommended default:** do NOT reuse `0x230`
> (taken). Implement the split on the *already-reserved-for-async* pair
> `SYS_DTR_SUBMIT`=`0x240` (make it return a job handle instead of blocking) and
> `SYS_DTR_WAIT`=`0x241` (currently a `-1` stub at `syscall.c:1109` — the natural
> home). Update `README.md:411` to read `0x240/0x241` and check the box.
> Tradeoff: the README's literal numbers change; but they were stale and collided.
> The *first slice* (Part II) does NOT need async — it uses synchronous
> `SYS_INFER` (`0x210`). Async is a later wave; this CDN just records the correct
> target so the implementer doesn't resurrect the collision.

### I.3 What the kernel must do on a ring-3 fault (the survival mechanism)

This is the heart of the change. Today `exception_handler` (`boot/x86/idt.c:85`)
treats every fault identically and halts. The substrate must instead distinguish:

- **Fault while CS = KERN_CS (0x08 / ring-0):** a real kernel bug → keep current
  behavior (print + halt). The substrate genuinely failed; halting is honest.
- **Fault while CS = USER_CS (0x23 / ring-3):** the *mind* faulted → do NOT halt.
  Instead: print a short diagnostic, **terminate the offending user task**
  (`tk_ter_tsk`/`tk_ext_tsk` on the current task), and **return control to the
  scheduler** so other tasks (shell, net, DMN-stub) keep running. The kernel may
  then restart the core.

The discriminator is the saved CS in the interrupt frame the CPU pushed. The
ring-3→ring-0 entry frame layout is documented in `isr.S:291-307`; on a *fault*
from ring-3 the CPU pushes `[SS, RSP, RFLAGS, CS, RIP(, errcode)]`. The handler
reads the pushed CS and branches. This requires the exception ISR stubs
(`isr6`/`isr13`/`isr14`, `isr.S:50,88,95`) to pass the saved CS to
`exception_handler`, which today only receives `(exception_num, error_code)`
(`idt.c:85`). **This is the one piece of substrate code that genuinely must
change** — it is part of the immutable substrate, but it is *currently wrong* for
the new world (it cannot survive a tenant fault). Everything else is reuse.

---

## Part II — the first slice (the smallest falsifiable thing)

### II.1 What gets built

**(a) ONE core computation in a ring-3 user task on bare-metal x86.**

Pick the simplest real core entry: **`moe_infer(B temp,B hum,B press,B light)`**
(`arch/common/moe.c`, `moe.h:164`), which returns a class in `[0, MOE_NUM_CLASSES)`
= `[0,3)` (`moe.h:32`). It is pure-ish (gate → reflex → return), deterministic for
fixed input, and already has observability (`moe_infer_last`, `moe.h:171`).

The ring-3 user task is a tiny native ELF (built like the existing
`boot/x86/user_hello/`), loaded by the **existing** `elf_exec()`
(`arch/x86/elf_loader.c:201`) which already builds the ring-3 IRET frame via
`user_exec()` (`arch/x86/userspace.c:26`). The user task issues `SYS_INFER`
(`0x210`) with a packed sensor word; the kernel thunk at `syscall.c:960` calls
`mlp_forward` today — for the slice it is repointed/aliased so the *ring-3 path*
exercises the moved `moe_infer`. Concretely the slice ships a user binary
`core_moe.elf` that:

1. calls `SYS_INFER(SENSOR_PACK(t,h,p,l))` for a fixed test vector,
2. prints the returned class via `SYS_WRITE(1, …)`,
3. on success calls `SYS_EXIT(0)`.

> Note — the directive's deeper goal is to run the *actual* `moe_infer` body in
> ring-3, not merely to syscall a still-ring-0 `mlp_forward`. For the FIRST slice
> the falsifiable claim is narrowed to: *the moe class-inference computation
> executes in a ring-3 task and a deliberate fault in that task does not kill the
> kernel.* Relocating the full `moe.c` body to link into the user ELF (vs. behind
> the syscall) is **CDN-4 option (a)** and is the first thing the *next* slice
> widens. The acceptance gate (II.3) is written to be honest about which is being
> proven.

**(b) Kernel-survives-core-crash proof.**

Add a second user binary `core_crash.elf` (or a `--crash` arg to `core_moe.elf`)
that, after one successful infer, deliberately faults in ring-3 by **a null
dereference**: `*(volatile int*)0 = 0;`. (USER page tables make address 0 not
present → `#PF` from ring-3.) An alternate inducer is a `ud2` bad opcode → `#UD`
from ring-3; both are acceptable, null-deref is the default because it also
exercises the page-fault path the real core is most likely to hit.

The kernel response (per I.3): the modified `exception_handler` sees the fault came
from `CS=USER_CS`, prints e.g. `[core] ring3 fault #14 @<rip> — task reaped`,
terminates the faulting task, and **returns to the scheduler**. The shell prompt
must come back and accept input; net/SWIM keep ticking. The kernel may then
re-`elf_exec` the core.

**How survival is observed (operationally):**

1. From the shell, run the crash binary; observe the `[core] ring3 fault … reaped`
   line and that the shell prompt returns.
2. Type a normal shell command (e.g. `ver` / `help`) and get output → scheduler
   alive.
3. Re-launch `core_moe.elf` and get the *same* correct class as before the crash →
   restart works and the substrate (incl. any durable weights) is intact.

### II.2 Anti-fork reuse surface

The implementer **REUSES** (does not re-create):

- `arch/x86/userspace.c` `user_exec()` — the ring0→ring3 IRET trampoline.
- `arch/x86/gdt_user.c` + `gdt_user.h` — `USER_CS=0x23`, `USER_DS=0x2B`,
  `TSS_SEL`, `gdt_set_kernel_stack()`. The user memory map
  (`USER_CODE_BASE 0x400000`, `USER_STACK_TOP 0x1000000`) is fixed and reused.
- `arch/x86/elf_loader.c` `elf_exec()` + `user_launcher()` + `build_argv_stack()`
  — loading and launching the user ELF.
- `arch/x86/syscall.c` `syscall_dispatch()` + the `int 0x80` gate — extend the
  switch; do not add a second dispatcher.
- `boot/x86/isr.S` `syscall_isr` transition frame — reuse; only the *exception*
  stubs (`isr6`/`isr13`/`isr14`) gain a saved-CS argument.
- `arch/common/moe.c` `moe_infer()` — the computation; do not fork a "ring3 moe".
- The `boot/x86/user_hello/` build pattern + `boot/x86/Makefile` user-ELF rules.

**Do NOT create a parallel X:**

- No second syscall table / dispatcher / IDT gate.
- No second GDT, TSS, or ring-3 trampoline.
- No copy of `moe.c` / `dtr.c` math under `arch/x86/`.
- No new "user libc" — reuse whatever `user_hello` links against.
- No second exception handler — *modify* `exception_handler` in place.

### II.3 The acceptance gate (CI) — falsifiable, fake-resistant

A new shell verb **`ring3 test`** (hook into the `shell.c` dispatch chain next to
`cmd_dmn`/`cmd_self`, around `arch/x86/shell.c:2138`) runs the full sequence
in-process and prints exactly one terminal line. The separate AUDIT agent re-runs
it from scratch (boot QEMU, type `ring3 test`).

The verb performs, in order, and PASSES iff **all four** hold:

1. **R3-INFER:** `elf_exec("core_moe.elf")` runs in ring-3 and `SYS_INFER` returns
   the expected class `C0` for fixed input `V0`. (Expected `C0` is captured by
   calling `moe_infer` once in ring-0 first and comparing — the gate is "ring-3
   result == ring-0 result", so it cannot be greened by hard-coding a constant.)
2. **CRASH-CAUGHT:** `elf_exec("core_crash.elf")` faults in ring-3; the kernel
   prints a fault line whose recorded `from_ring == 3` (saved `CS == USER_CS`),
   and `exception_handler` **returns** (does not `hlt`). A monotonic counter
   `ring3_faults_reaped` increments by exactly 1.
3. **SCHED-ALIVE:** after the crash, a sentinel ring-0 task that was sleeping
   (`tk_dly_tsk`) before the crash is observed to have woken and bumped a counter
   AFTER the crash timestamp → the scheduler kept running. (Not just "shell
   printed" — a counter that advances *post-crash* is harder to fake.)
4. **RESTART-OK:** a second `elf_exec("core_moe.elf")` after the crash returns the
   **same** class `C0` for `V0` → the substrate + weights survived and the mind
   restarts.

**Exact pass formula (the single printed line):**

```
ring3: PASS  infer=C0 ring3==ring0:Y  reaped=1 from_ring=3  sched_post=Y  restart=C0:Y
```

PASS is printed **iff**:

```
(r3_class == r0_class)                       /* gate 1: ring3 infer == ring0 oracle */
  && (ring3_faults_reaped == 1)              /* gate 2: exactly one reap, no double-fault */
  && (last_fault_from_ring == 3)             /* gate 2: it was a ring-3 fault, not ring-0 */
  && (exception_handler returned, no hlt)    /* gate 2: substrate did not halt          */
  && (sentinel_ticks_after_crash > sentinel_ticks_at_crash)  /* gate 3: scheduler alive */
  && (restart_class == r0_class)             /* gate 4: restart reproduces the oracle    */
```

Any single failure prints `ring3: FAIL <which-gate>` and the verb returns nonzero.
Threshold values: `C0` is whatever `moe_infer(V0)` returns in ring-0 at run time
(not hard-coded). `reaped == 1` exactly (not `>= 1`) so a crash *storm* or a
double-fault halt cannot masquerade as a pass. `from_ring == 3` exactly so a
*kernel* bug accidentally surviving cannot green it.

This is fake-resistant: you cannot pass gate 1 by returning a constant (it is
compared to a live ring-0 call), cannot pass gate 2 by widening `>= 1`, cannot pass
gate 3 with a print (it needs a counter that advanced *after* the crash), and
cannot pass gate 4 without the durable substrate actually surviving.

### II.4 Honest bounds — what this slice does NOT do

- **Only one ring-3 module.** It proves `moe_infer`, not the whole core. `dtr`,
  `lm`, `dmn`, `gl`, `reflex`, `r3` stay ring-0 until later waves (CDN-4).
- **Possibly still syscalls into ring-0 math.** Per II.1's note, the first slice
  may exercise `moe_infer` *behind* `SYS_INFER` rather than linking the full
  `moe.c` body into the user ELF. The gate is written to be honest about this; the
  *next* slice widens to linking the body into ring-3.
- **No async infer.** Synchronous `SYS_INFER` only; the `0x240/0x241` async split
  (CDN-5) is deferred.
- **Coarse fault isolation.** One mind = one address space; a fault kills the whole
  mind (kernel survives). Per-module isolation deferred (CDN-4 option b).
- **Known risks / corner cases:**
  - *TSS.RSP0 freshness* — `user_exec` sets `RSP0 = isstack` (`userspace.c:35`).
    On a ring-3 fault, the CPU loads RSP0; if a previous user task's RSP0 is stale,
    the fault handler's own stack is wrong. The handler must re-establish a known
    kernel stack before doing work, and `gdt_set_kernel_stack` must be called per
    task switch (it already is, `userspace.c:35`).
  - *Syscall re-entrancy / partial state* — if the core faults *inside* a syscall
    (kernel holds a lock, half-written p-fs block), reaping the user task must not
    leave a kernel lock held. The existing `fs_ssy` cleanupfn path
    (`syscall.c:39-42`, per-task FD ownership) is the model; weights writes must be
    crash-atomic at the p-fs layer (ties to CDN-2's "batch at consolidation
    boundary").
  - *Shared-page lifetime* — tensors/buffers the core handed the kernel by pointer
    (`SYS_TOPIC_SUB` etc.) must not be freed under the kernel while a syscall is
    mid-flight; reaping happens at fault time, not mid-syscall, so the existing
    "dereference under user CR3 synchronously inside the syscall" pattern is safe.
  - *`#DF` (double fault)* — if the ring-3 fault handler itself faults (bad RSP0),
    the CPU escalates to `#DF` (`isr8`) which has no recovery; the gate's
    `reaped == 1` exact-match catches this (a double fault would not increment the
    reap counter, so the gate FAILs rather than silently passing).

### II.5 aarch64 EL0 sketch (mirror slice, NOT in the first wave)

aarch64 is a **sketch only** — there is *no* EL0/SVC scaffolding today (verified:
`arch/aarch64/` has `start.S`, `cpu_support.S`, IRQ vectors via `_vec_el1_irq`, but
no EL0 entry, no `svc` handler, no user GDT-equivalent). To mirror the x86 slice
later, the missing pieces are:

1. **EL1→EL0 entry trampoline** (the `user_exec` analogue): set `SPSR_EL1` to
   select EL0 (M[3:0]=0), put the user entry in `ELR_EL1`, the user stack in
   `SP_EL0`, then `eret`. (x86 `userspace.c` builds an IRET frame; aarch64 builds
   the `SPSR_EL1`/`ELR_EL1` pair and `eret`s.)
2. **`svc` handler** (the `int 0x80` analogue): an EL1 synchronous-exception vector
   entry that decodes `ESR_EL1.EC == 0x15` (SVC from AArch64), reads the syscall
   number from `x8` (or a chosen register) and args from `x0..x2`, and calls the
   *same* `syscall_dispatch(nr, arg0, arg1, arg2)`. The calling convention maps
   `x8→nr, x0→arg0, x1→arg1, x2→arg2, x0=ret`.
3. **EL0/EL1 stack split** — analogous to TSS.RSP0: on EL0→EL1 the CPU uses
   `SP_EL1`; the vector must run on a valid EL1 stack (already true for IRQ; reuse).
4. **The survival branch** — the EL1 synchronous-exception vector must, on an
   *EL0-originated* fault (data abort `EC=0x24`, instruction abort `EC=0x20`,
   illegal/unknown), read `SPSR_EL1.M` to confirm the faulting EL was EL0, reap the
   user task, and return to the scheduler — exactly the x86 `from_ring==3` branch.
   (Read the existing `_vec_el1_*` vector layout in `arch/aarch64/start.S` /
   `cpu_support.S` first; the AArch64 IRQ-path C-ABI traps are documented in the
   project memory — suspect the vector before the device.)

The aarch64 acceptance gate, when built, is the *same four-gate formula* (II.3)
with `from_ring==3` replaced by `from_EL==0` (`SPSR_EL1.M == 0`).

---

## Sequencing

- **Wave A (this doc):** design only. No code.
- **Wave B (first slice):** II.1 + II.3 on x86. Separate implementer + auditor.
  Touches: `boot/x86/idt.c` (`exception_handler` gains saved-CS + survival branch),
  `boot/x86/isr.S` (exception stubs pass saved CS), `arch/x86/shell.c`
  (`ring3 test` verb), and a `core_moe.elf` / `core_crash.elf` under
  `boot/x86/user_hello/`-style rules. The auditor re-derives the gate formula
  line-by-line (per the project's "commander reads the gate formula" discipline)
  and re-runs from a cold boot.
- **Wave C+:** widen to full `moe.c` body in ring-3 (CDN-4a→full), then `dtr`/`lm`,
  then async infer (CDN-5), then per-module address spaces (CDN-4b), then the
  aarch64 EL0 mirror (II.5).

---

## Part III — Wave C: the mind's math moves into ring3

> Status: **SHIPPED (Wave C — the mind's math runs in ring3)** — wave-27; 2026-06-19
> doc-status fix. The "written before implementation" line below is STALE. What shipped:
> `samples/12_ring3/03_core_mind/core_mind.c` dual-compiles the SAME `moe.c`+`dtr.c` into
> `core_mind.elf` (a 14-symbol zero-math shim), weights flow via 0x213 and visibility via
> 0x214; proven by `kernel_infer_count` delta==0 (all routes counted in ring3) + a user-copy
> poison flipping the answer + an `nm` tripwire. Builds ON the shipped Wave B: the survival branch
> in `boot/x86/idt.c:153-173`, `user_fault_reap` (`arch/x86/syscall.c:154`),
> the `ring3 test` verb (`arch/x86/shell.c:1422-1540`), the user ELFs under
> `samples/12_ring3/`, and the `ring3-survival` CI job
> (`.github/workflows/ci.yml:261-302`).

### III.0 The claim — and what Wave B honestly did NOT claim

Wave B proved: *a ring-3 task crash does not kill the kernel, and a ring-3
task can obtain a correct class through `SYS_INFER`.* The math itself still
executed **in ring-0**: the `SYS_INFER` thunk (`arch/x86/syscall.c:1007-1020`)
calls `moe_infer` kernel-side, and the thunk's own comment says so
("The math still executes in ring-0 behind this syscall for THIS slice").
Comparing the `SYS_INFER` result against a ring-0 oracle (Wave B gate 1)
proves *correctness of the channel*, not *where the computation ran* — both
sides were the same ring-0 function.

**Wave C's claim:** the class-inference computation — `moe_infer`'s
gate/route/return and the full dtr Transformer forward it drives — **executes
inside a ring-3 user task, on a user-space copy of the weights, with zero
kernel-side inference during the run.** This is the directive made literal:
the 常に変化していく領域 (the learned weights and the math that consumes
them) now lives in user space; the kernel supplies only weights, console,
and the fault net.

The gate (III.4) is constructed so that a ring-0-computed answer **cannot**
green it (III.3).

### III.1 The link cut

#### III.1.1 `moe_infer`'s full call graph, with a disposition for every edge

Read from `arch/common/moe.c` (entry `moe_infer`, `moe.c:431-496`):

| Dependency | Site | What it is | Disposition in the user ELF |
|---|---|---|---|
| `dtr_decide` → `dtr_forward_probs` | `moe.c:128-143` → `dtr.c:1004-1010` | THE math: one dtr forward, argmax + max-softmax conf | **LINK IN** (the point of the wave) |
| `train_forward` (+ `tc` stash) | `dtr.c:741`, `tc` at `dtr.c:695` | the actual forward used by `dtr_forward_probs`; self-contained float math over the static weight arrays (`dtr.c:204-227`) | **LINK IN** (comes with `dtr.c`) |
| dt math kernels (`dtr_expf`/`dtr_logf`/`dt_sqrt`/`dt_linear`/`dt_softmax`) | `dtr.c:100-176` | libc-free float primitives | **LINK IN** |
| `ret_set` / `ret_blend` | `retrieval.c:193` → `pfs_dag_read` (`retrieval.c:167`) | p-fs engram retrieval — a KERNEL durable-state service | **STUB** (no-op). Semantics-preserving: `dtr_forward_probs` forces retrieval OFF around the forward (`ret_set(0)`, `dtr.c:1006`), so a no-op stub computes the IDENTICAL function. Honest note: a future ring-3 retrieval goes through `SYS_READ`, not a re-implementation. |
| `select_expert` | `moe.c:278-374` | routing: reads gossip state — `region_recompute`/`region_is_member` (`moe.c:294,317`), `swim_rtt_ms` (`moe.c:309`), `world_peer_pressure`/`world_peer_threat` (`moe.c:223-241`), `reflex_threat_level` (`moe.c:316`), `dnode_table` (`moe.c:304`), `drpc_my_node` (`moe.c:286`) | **LINK IN the function, STUB its kernel-state inputs.** With the shim's `drpc_my_node = 0xFF`, `select_expert` takes the early local-only return at `moe.c:286-291` ("reflex local-only") BEFORE touching any of the stubbed symbols — the stubs satisfy the LINKER but are never executed this wave. Routing degenerates to local, stated in III.5. |
| `world_note_firing` | called unconditionally, `moe.c:446` | observability: lights the node's firing indicator in the world map | **SYSCALL-BACK** via new `SYS_MIND_NOTE` (CDN-7; stub fallback). 可視化 is a mechanism, not decoration — a ring-3 inference should still light the map. |
| `reflex_on_inference` | called unconditionally, `moe.c:486` | the GUARD: inference completion drives the reflex/defense layer | **SYSCALL-BACK** via the same `SYS_MIND_NOTE` (CDN-7). The reflex action table manipulates kernel/world state and stays ring-0; the *hook* crosses the boundary. |
| `dtk_infer` (remote expert) | `moe.c:465` | drpc remote inference | **STUB** (returns error → fallback path). Never executed this wave (local-only early return means `expert == me`). Remote delegation from ring-3 is a later wave (CDN-3 transport syscalls). |
| `mo_puts` → `sio_send_frame` | `moe.c:51-61` | console output | **SHIM** → `SYS_WRITE(1, buf, n)` (`plibc.h` already wraps it) |
| `my_total++` / `my_correct` / `my_accuracy` | `moe.c:475,506-511` | local score state | **LINK IN** (private copy). Feedback/score gossip stays ring-0 (`moe_task`, `moe.c:538-596` — not linked, see GC note). |
| `kdds_*` (score topics) | only in `moe_task`/`broadcast_score` (`moe.c:395-407,538-596`) | Collective transport | **GC'd** — unreachable from `moe_infer`; `--gc-sections` drops the sections, so `kdds_*` needs no stub at all. |
| `gl_merge` | only in `moe_self_test` (`moe.c:1293`) | gossip learning | **GC'd** |
| `dkva_cache_update` | `dtr.c:478` inside `run_mhsa_local` (`dtr.c:458`) | distributed KV cache | **GC'd** — NOT on the user path: `dtr_forward_probs` goes through `train_forward`, not `run_transformer_local` (`dtr.c:569`). Verified: `train_forward`'s only external call is `ret_blend` (`dtr.c:810`). |
| `tm_putstring` (`dt_puts`, `dtr.c:51`) | `dtr_stat`/init paths | T-Monitor console | **GC'd** (those sections unreachable); if a stray reachable reference appears, shim to `SYS_WRITE`. |

So the user ELF (**`core_mind.elf`**) links: `arch/common/moe.c` (whole file,
dead sections GC'd) + `arch/common/dtr.c` (whole file, dead sections GC'd) +
`samples/12_ring3/03_core_mind/core_mind.c` (the `_start`/driver) +
`userland/x86/ushim.c` (the shim — an INTERFACE layer like `plibc.h`,
containing **zero math**).

**Anti-fork rule, restated as a build invariant:** the .text of the forward
in the kernel and in `core_mind.elf` comes from the SAME `arch/common/moe.c`
and `arch/common/dtr.c`. No file under `samples/` or `userland/` may contain
a transformer, a softmax, or a gate. The shim may only: set data symbols,
return constants, or issue `int 0x80`.

#### III.1.2 The dual-compile mechanism (one source → kernel .o + user .o)

Kernel side already builds `moe.c`/`dtr.c` via `boot/x86/Makefile:260-261`
(`$(COMMON_OBJS): %.o: $(COMMON)/%.c`) with CFLAGS at `boot/x86/Makefile:36-40`
(`-m32 -ffreestanding -fno-stack-protector -fno-pic -fcf-protection=none
-O1 -std=gnu11 -D_APP_X86_` + INCDIRS at `:23-30`). The user sample build
(`userland/x86/Makefile:28-33`) uses the *same* flag family minus
`-D_APP_X86_` and the kernel include dirs.

Wave C adds to `userland/x86/Makefile` a dedicated rule (sketch — the
implementer writes the real one):

```make
ROOT       = ../..
CORE_INC   = -I$(ROOT)/arch/x86/include -I$(ROOT)/arch/common/include \
             -I$(ROOT)/relay -I$(ROOT)/include/kernel/tkernel \
             -I$(ROOT)/include -I$(ROOT)/include/lib/libc
CORE_CFLAGS = $(CFLAGS) -D_APP_X86_ $(CORE_INC) \
              -ffunction-sections -fdata-sections
CORE_DIR   = 12_ring3/03_core_mind

$(CORE_DIR)/core_mind.elf: $(S)/12_ring3/03_core_mind/core_mind.c \
                           $(ROOT)/arch/common/moe.c $(ROOT)/arch/common/dtr.c \
                           ushim.c plibc.h user.ld
	mkdir -p $(CORE_DIR)
	$(CC) $(CORE_CFLAGS) -c $(S)/12_ring3/03_core_mind/core_mind.c -o $(CORE_DIR)/core_mind.o
	$(CC) $(CORE_CFLAGS) -c $(ROOT)/arch/common/moe.c  -o $(CORE_DIR)/moe_user.o
	$(CC) $(CORE_CFLAGS) -c $(ROOT)/arch/common/dtr.c  -o $(CORE_DIR)/dtr_user.o
	$(CC) $(CORE_CFLAGS) -c ushim.c                    -o $(CORE_DIR)/ushim.o
	$(LD) $(LDFLAGS) --gc-sections -o $@ $(CORE_DIR)/core_mind.o \
	      $(CORE_DIR)/moe_user.o $(CORE_DIR)/dtr_user.o $(CORE_DIR)/ushim.o
```

The load-bearing pieces:

- **Separate build dir** (`userland/x86/12_ring3/03_core_mind/*.o`): the
  kernel's `moe.o`/`dtr.o` live under `boot/x86/`; no clobbering, no
  recursive-make races (`boot/x86/Makefile` already invokes
  `$(MAKE) -C $(USERLAND_X86)` inside the `disk` target).
- **`-D_APP_X86_` + the kernel INCDIRS**: typedef widths (`W`/`UW`/`B`) and
  struct layouts must be bit-identical to the kernel build, or the fetched
  weights blob and `SENSOR_PACK` formats silently diverge (the LP64 typedef
  trap, x86 edition — here both sides are ILP32 so it is cheap to keep true).
  Including kernel *headers* is fine and is not "kernel leaking into user":
  headers are shared declarations; only the shim decides what each symbol
  *does* in ring-3.
- **`-ffunction-sections -fdata-sections` + `ld --gc-sections`**: this is
  what makes whole-file dual-compile viable. References inside discarded
  sections (e.g. `kdds_pub` from `broadcast_score`, `dkva_cache_update` from
  `run_mhsa_local`, `tk_cre_sem` from `dtr_init`) do not need definitions.
  `user.ld` has `ENTRY(_start)` and no `KEEP()` directives, so the live set
  is exactly what `_start` reaches. The implementer should build once with
  `--print-gc-sections` to confirm the survivor list matches III.1.1.
- **`$(LIBGCC)`** may be needed for 64-bit division helpers; add it the way
  `boot/x86/Makefile:41` does if the link asks for it.

Fallback (CDN-10): if the first link reveals the shim surface is much larger
than III.1.1's table (i.e. `train_forward`'s section graph drags in kernel
services this audit missed), the fallback is a *mechanical file split* of
`dtr.c` into `dtr_forward.c` (math + weights + `tc`) and `dtr.c`
(orchestration), both kernel- and user-linked — a move, never a copy.
Do NOT silently fall back: that is a commander decision.

#### III.1.3 The shim surface (`userland/x86/ushim.c`) — exhaustive expected list

Symbols the live (non-GC'd) sections reference that the kernel normally
provides. Disposition key: SYSCALL = forwards to `int 0x80`; STUB-DEAD =
exists only to satisfy the linker, unreachable at runtime behind the
`drpc_my_node==0xFF` guard (`moe.c:286-291`); STUB-LIVE = executed, must be
semantics-preserving.

| Symbol | Kind | Disposition |
|---|---|---|
| `sio_send_frame` | code | SYSCALL → `SYS_WRITE(1,...)` (live: all `[moe]` prints) |
| `drpc_my_node` | data (UB) | defined `= 0xFF` (single-node identity; THE guard) |
| `dnode_table` | data | zero-initialized array (STUB-DEAD) |
| `swim_rtt_ms` | code | STUB-DEAD, returns `0xFFFFFFFF` |
| `world_peer_pressure` / `world_peer_threat` | code | STUB-DEAD, return `-1` |
| `region_recompute` / `region_is_member` | code | STUB-DEAD, no-op / 0 |
| `reflex_threat_level` | code | STUB-DEAD, returns 0 |
| `dtk_infer` | code | STUB-DEAD, returns error (≠`E_OK`) |
| `world_note_firing` | code | SYSCALL-BACK `SYS_MIND_NOTE(op=0, cls)` — CDN-7 |
| `reflex_on_inference` | code | SYSCALL-BACK `SYS_MIND_NOTE(op=1, cls|conf<<8)` — CDN-7 |
| `ret_set` / `ret_blend` | code | STUB-LIVE: `ret_set` returns 0, `ret_blend` no-op — identical to the forced-OFF semantics at `dtr.c:1006` |

If the first link demands anything beyond this table, the implementer stops
and reports — that is the early-warning that the cut is bigger than designed.

#### III.1.4 The user program (`samples/12_ring3/03_core_mind/core_mind.c`)

One fat ELF, three modes selected by argv (the argv frame already exists:
`build_argv_stack`, `arch/x86/elf_loader.c:117-133,136` — `_start` captures
entry ESP with two instructions of inline asm; argc at `[esp+0]`, argv at
`[esp+4]`; Wave B never used it, Wave C does):

1. **normal** (no arg): fetch weights (III.2) → `dtr_weights_set(wbuf)` into
   the ELF's own static arrays → `cls = moe_infer(V0)` **in ring-3** →
   print → `sys_exit(cls)`. Exit code = the Wave B gate-1 channel
   (`user_last_exit_code()`, `shell.c:1474`).
2. **`-poison`**: fetch + install weights; compute `c_pre = moe_infer(V0)`;
   perturb the **user copy**: `wbuf[632 + c_pre] -= 50.0f` (the flat-buffer
   index of `b_cls[c_pre]` — `b_cls` is the last 3 floats of the 635-float
   layout, `dtr_weights_get` order, `dtr.c:302-324`); `dtr_weights_set(wbuf)`
   again; compute `c_post = moe_infer(V0)`; `sys_exit((c_pre << 4) | c_post)`.
   A −50 logit floor dominates trained logits (observed O(±10)), so the
   argmax MUST leave `c_pre` — and only a computation actually running on
   the user-space copy can feel it.
3. **`-crash`**: fetch + install + one successful `moe_infer(V0)`, then
   `*(volatile int *)0 = 0;` — the Wave B inducer (`core_crash.c`), now
   inside the FAT ELF, proving the bigger mind is still reapable.

V0 = the Wave B vector (`R3_V0_*`, `shell.c:1387-1390`), shared by
`core_moe.c` — keep the single definition discipline.

### III.2 The weights channel — CDN-6

The ring-3 core must compute on the LIVE learned weights (else gate M1's
"== ring-0 oracle" comparison is vacuous). Two candidate channels:

- **(A) — recommended: a new small syscall `SYS_DTR_WEIGHTS_GET = 0x213`.**
  `arg0` = user `float*` buffer, `arg1` = capacity in floats. Kernel thunk:
  validate `arg1 >= DTR_WEIGHT_FLOATS`, call the existing
  `dtr_weights_get(buf)` (`dtr.c:302-324`) directly into the user pointer
  (the established synchronous-deref pattern, e.g. `PK_TOPIC_SUB` at
  `syscall.c:1121-1128`), return `DTR_WEIGHT_FLOATS`. Size:
  `DTR_WEIGHT_FLOATS = 635` (`dtr.h:174`) × 4 B = **2,540 bytes**; a single
  memcpy-scale copy, microseconds — no latency design needed. `0x213` is
  free (`p_syscall.h:316-318` ends at `0x212`; topics start `0x220`).
- (B) `SYS_READ` on the p-fs weights file (the `DTR_WFILE` blob format,
  `dtr.h:248-267`, written by `dtr_train.c:299`). Rejected for the slice:
  the file is a *persistence point* snapshot, not the live weights —
  after any `lm`/`gl_merge`/training activity the gate would honestly FAIL
  on staleness, which is the wrong thing to be testing.

**Staleness honesty:** even (A) is a snapshot at fetch time. For ONE
inference that is exactly right (it is the same consistency a ring-0 caller
gets between two `gl_merge`s). A long-lived ring-3 mind needs either
re-fetch-per-decision (cheap: 2.5 KB) or the shared read-only mapping that
belongs to the per-module-address-space wave (CDN-4b). Flagged, not built.

### III.3 The proof the math ran in ring3 — the fake-resistant core

Wave B's comparison cannot distinguish "computed in ring-3" from "asked
ring-0". Wave C uses three mechanisms; (a)+(b) at runtime, (c) static. All
three ship.

**(a) The kernel-compute counter (must stay frozen).** A new global
`volatile UW kernel_infer_count` (declared in `dtr.h`, defined in `dtr.c`),
incremented at the entry of EVERY kernel-resident compute path that can
produce a class:

- `train_forward` (`dtr.c:741`) — covers `dtr_forward_probs`/`dtr_classify`
  /`moe_infer`/drpc `DRPC_CALL_INFER`/`gl`/`lm` paths;
- `run_embed_seq` (`dtr.c:391`) — covers `run_transformer_local`
  (`SYS_DTR_SUBMIT` solo), `run_stage0` pipeline entry, KV seeding;
- `mlp_forward` (`ai_job.c`) — covers `SYS_AI_SUBMIT`/`SYS_AI_WAIT` and
  `edf_infer` (`edf.c:122` calls `mlp_forward` locally).

These increments compile into the KERNEL objects; the user ELF's own copy of
the same source increments its own private counter in its own .data — no
interference. The gate snapshots the kernel counter after the oracle call
and requires **delta == 0** across every ring-3 run. Any implementation that
secretly answers via `SYS_INFER`/`SYS_DTR_SUBMIT`/`SYS_INFER_SLA`/
`SYS_AI_*` bumps the counter and FAILS. The shell verb reads the counter
directly (the shell is ring-0; no syscall needed).

*Why delta==0 is stable:* the Wave B quiesce already pauses the ELF
watchdog and kills `infer_d.elf` (`shell.c:1441-1443`); `moe_task`'s loop is
integer-only gossip (`moe.c:563-595`); `dmn_task` calls only `dtr_stat`
(`dmn.c:127` — prints counters, no forward); `lm`/`gl`/training run only
from shell verbs. Single-node CI has no drpc peers. Any FUTURE background
inference task will trip this clause loudly — that is a feature, and the
fix is to quiesce it in the verb's preamble, never to widen `== 0`.

**(b) The poison test (proves the ring-3 COPY is what computes).** The
`-poison` run perturbs only the USER-SPACE weight copy; the kernel weights
are untouched. Clause: `c_pre == oracle && c_post != oracle`. A ring-0-
computed answer tracks the kernel weights and CANNOT change when the user
copy is poisoned; a hard-coded pair cannot know the live oracle class at
build time (it is whatever the trained/merged weights say at run time, per
Wave B's no-constant discipline at `shell.c:1383-1386`). Together with (a),
the ONLY green path is: fetch live weights → compute locally → feel the
poison. The restored proof is gate M7: a fresh normal run matches the
oracle again (the poison did not leak back into the kernel).

**(c) Static link check (cheap CI tripwire).** After building the disk:

```
i686-linux-gnu-nm userland/x86/12_ring3/03_core_mind/core_mind.elf \
  | grep -E ' T (moe_infer|dtr_forward_probs|dtr_weights_set)$'
```

All three are global (non-static) and reachable, so they survive
`--gc-sections`; a build that links a stub forward instead of the real one
loses `dtr_forward_probs`/`moe_infer` from the live set and fails the grep.
This check is necessary-not-sufficient (symbols could exist uncalled) —
the runtime clauses (a)+(b) are the fence; (c) catches build rot one minute
into CI instead of fifteen.

*Excluded cheat, stated honestly:* on a MULTI-node mesh a cheater could ask
a peer (`dtk_infer`) without bumping local kernel counters. CI is
single-node (no peers alive), and the poison clause independently excludes
it (a remote answer cannot track the local user copy). Both mechanisms are
required; neither alone suffices.

### III.4 The acceptance gate — `ring3 mind`

New sub-verb of the existing `cmd_ring3` (`shell.c:1422` — extend the arg
parse; `ring3 test` remains byte-for-byte the Wave B regression). Preamble
reuses Wave B's quiesce verbatim (`heal_elf_pause(TRUE)`,
`dproc_kill_by_name("infer_d.elf")`, settle delay — `shell.c:1441-1443`) and
the sentinel task (`shell.c:1392-1405`). Sequence and clauses — every
comparison exact (`==`, never `>=`):

```
quiesce; start sentinel
r0  = moe_infer(V0)                      /* ring-0 oracle (bumps counter) */
k0  = kernel_infer_count                 /* snapshot AFTER the oracle     */
reaped0 = ring3_faults_reaped

M1  run core_mind.elf            → exit m1;        m1 == r0
M2  k1 = kernel_infer_count;                       k1 - k0 == 0
M3  run core_mind.elf -poison    → exit p;
        c_pre  = (p >> 4) & 0xF;                   c_pre  == r0
        c_post =  p       & 0xF;                   c_post != r0
M4  k2 = kernel_infer_count;                       k2 - k1 == 0
M5  run core_mind.elf -crash     → exit c;
        ring3_faults_reaped - reaped0 == 1
        last_fault_from_ring          == 3
        c                             == R3_EXIT_FAULT (-86)
M6  ticks_after > ticks_at_crash                   /* sentinel advanced  */
M7  run core_mind.elf            → exit m7;        m7 == r0
M8  k3 = kernel_infer_count;                       k3 - k2 == 0
        /* M5/M7's in-ELF forwards ran in ring-3; the crash run's clean
           forward and the restart bump NOTHING kernel-side */
```

PASS prints exactly:

```
ring3-mind: PASS  infer=C ring3==ring0:Y  kdelta=0/0/0  poison=PRE->POST:Y  reaped=1 from_ring=3  sched_post=Y  restart=C:Y
[ring3-mind] PASS
```

Any failure prints `ring3-mind: FAIL <clause-id>` with the raw values
(Wave B's diagnostic style, `shell.c:1527-1538`) and `[ring3-mind] FAIL`.

Exactness rationale, clause by clause: `m1 == r0` (live oracle, no
constant); `k-deltas == 0` exactly (one stray kernel forward = FAIL — a
`<= 1` would readmit the Wave B world); `c_pre == r0` (the poison run must
first prove it computes correctly, else a broken forward whose garbage
"changes" would pass); `c_post != r0` (the only inequality, and it is the
poison's job); `reaped delta == 1` and `from_ring == 3` and
`exit == -86` (`R3_EXIT_FAULT`, `shell.c:1381` / `USER_EXIT_FAULT`,
`syscall.c:115`) — the full Wave B crash triple re-proven against the FAT
ELF; `m7 == r0` (restart + poison-did-not-persist).

**Falsification paths (each must FAIL):**

1. Build links the shim/stub instead of the real forward → CI nm check (c)
   fails; if somehow linked-but-bypassed at runtime via `SYS_INFER`, M2
   fails (`kdelta != 0`).
2. `core_mind` hard-codes classes → M1/M3 cannot both hold without knowing
   the live `r0` (unknowable at build time).
3. Kernel computes, user relays (any syscall route) → M2/M4/M8 fail.
4. Poison hook removed / weights fetched but ignored → M3 `c_post != r0`
   fails (answer tracks kernel weights).
5. Fat ELF too big / fault path regressed → M5 triple fails exactly as in
   Wave B.

### III.5 Honest bounds — what Wave C still does NOT claim

- **Local-only mind.** `select_expert` degenerates to the `moe.c:286-291`
  local path (`drpc_my_node==0xFF` in the shim). The routing CODE is in
  ring-3; the routing INPUTS (SWIM RTT, world beacons, peer scores) are not
  yet fed across the boundary. Remote delegation (`dtk_infer`) from ring-3
  is deferred (CDN-3 transport syscalls are the path).
- **Weights are a fetched snapshot, ring-0-owned (CDN-2 default).** No
  shared mapping; no ring-3 weight WRITES. Training (`dtr_train_batch`),
  `lm_*`, `dmn`, `gl_merge`, GA all stay ring-0 this wave.
- **Retrieval/engram blend is OFF in ring-3** — identical to what
  `dtr_forward_probs` enforces ring-0-side (`dtr.c:1006`), so no behavior
  is lost; but a ring-3 mind with memory needs `SYS_READ`-backed retrieval
  later.
- **Sync `SYS_INFER` (0x210) is retained**, still computing ring-0, for its
  existing users: `net_infer.elf` (`samples/07_network/01_net_infer/`),
  `core_moe.elf`/`core_crash.elf` (the Wave B gate), and `edf_infer`'s SLA
  path; `infer_d.elf` uses `SYS_DTR_SUBMIT` (`infer_d.c:112`) — also
  unchanged (CDN-9). The kernel-compute counter intentionally measures
  these as kernel computes.
- **One address space; one mind** (CDN-4a). A fault kills the whole ring-3
  mind; kernel survives. Per-module spaces deferred (CDN-4b).
- **No async** — `0x240/0x241` SUBMIT/WAIT split (CDN-5) untouched.
- **aarch64 EL0 still future** (II.5 sketch unchanged).
- **FPU context risk (flagged, not fixed):** there is NO x87 save/restore
  anywhere in the x86 port (verified: zero `fxsave`/`fnsave`/`frstor` hits
  in `arch/x86/`, `boot/x86/`, `kernel/`). Ring-0 float math has survived
  because float users run sequentially; Wave C's gate keeps that true
  (oracle completes before the ELF runs; quiesce removes daemons; the
  sentinel and shell do no float). But a PREEMPTED ring-3 mind computing
  floats while any other task touches x87 corrupts silently. Before
  concurrent minds / background ring-3 inference, the dispatcher needs
  CR0.TS lazy switching or eager FXSAVE. This belongs in the gap ledger as
  its own entry.
- **Memory isolation is whatever Wave B established** — page 0 is U/S=0
  (per `core_crash.c` header) but the broader user/kernel split is not
  re-audited here; the poison clause does not depend on the kernel being
  unreadable from ring-3, only on the user copy being what computes.

### III.6 CI plan

**Extend the existing `ring3-survival` job** (`.github/workflows/ci.yml:261-302`)
rather than adding a second QEMU-boot job — same kernel, same disk, one
boot, two verbs; a second job would double the slowest path in CI for no
isolation gain (the verbs share nothing but the boot).

Concretely:

1. The stdin script at `ci.yml:287` gains the second verb:
   `( sleep 25; printf 'ring3 test\n'; sleep 90; printf 'ring3 mind\n'; sleep 90 )`
   and the poll loop waits for BOTH tags before killing QEMU.
2. The final gate at `ci.yml:302` becomes two required greps:
   `grep -aF '[ring3-survival] PASS' ring3.log` AND
   `grep -aF '[ring3-mind] PASS' ring3.log`.
3. A new cheap step right after "Build kernel + disk image" (`ci.yml:279`):
   the III.3(c) nm check (toolchain `gcc-i686-linux-gnu` already installed
   at `ci.yml:276`; `nm` ships with binutils alongside it).
4. `boot/x86/Makefile` disk target gains one line next to `:335-336`:
   `mcopy -i $@ $(USERLAND_X86)/12_ring3/03_core_mind/core_mind.elf ::core_mind.elf`
   (the recursive `$(MAKE) -C $(USERLAND_X86)` in the disk rule already
   rebuilds userland).

Rename the job's display name to
`ring3 survival + mind gate (boot QEMU x86)` so the two claims are visible
in the checks UI.

### III.7 COMMANDER DECISION NEEDED — the genuine forks

> **CDN-6 — weights channel.** (A) new `SYS_DTR_WEIGHTS_GET = 0x213` over
> live `dtr_weights_get` [recommended — the gate compares against the live
> oracle] vs (B) `SYS_READ` of the p-fs `DTR_WFILE` blob [no new syscall,
> but tests staleness instead of the mind]. III.2 has the full argument.

> **CDN-7 — the guard/observability hooks from ring-3.** `moe_infer`
> unconditionally calls `world_note_firing` (`moe.c:446`) and
> `reflex_on_inference` (`moe.c:486`). (a) one new syscall
> `SYS_MIND_NOTE = 0x214` (arg0=op, arg1=packed cls/conf; kernel side calls
> the two real functions) [recommended — ring-3 inferences keep lighting
> the world map and feeding the reflex; ~15 kernel lines] vs (b) no-op
> stubs + an honest-bounds line [zero new ABI, but a ring-3 inference
> becomes invisible to the guard until a later wave]. If (b), the gate is
> unaffected (neither hook influences the returned class), but 可視化
> regresses for ring-3 runs — the commander should weigh that against ABI
> growth.

> **CDN-8 — verb shape.** New sub-verb `ring3 mind` keeping `ring3 test`
> intact as the Wave B regression [recommended — two claims, two greppable
> tags, CI runs both] vs folding the new clauses into `ring3 test` [one
> verb, but a Wave C regression would mask which claim broke, and the Wave
> B tag's meaning would silently widen].

> **CDN-9 — does `infer_d.elf` switch to the ring-3 mind this wave?**
> Recommended: NO — it stays on `SYS_DTR_SUBMIT` (`infer_d.c:112`). The
> single user address space (CDN-4a, `elf_loader.c` single-`_uarg`) cannot
> host a resident `core_mind` daemon and the gate's transient execs at the
> same time; migrating the live daemon belongs with CDN-4b/CDN-5 (async +
> per-module spaces). Wave C proves the math relocation; it does not flip
> the fleet's serving path.

> **CDN-10 — dual-compile mechanism.** (a) whole-file compile of
> `moe.c`/`dtr.c` with `--gc-sections` + the III.1.3 shim [recommended —
> zero source-tree moves, the GC boundary IS the cut] vs (b) mechanical
> split of `dtr.c` into `dtr_forward.c` + orchestration [more invasive,
> but the right move if (a)'s first link demands stubs beyond the III.1.3
> table]. The implementer must not drift from (a) to (b) silently — a
> bigger-than-designed shim surface is a finding, not an inconvenience.

### III.8 Sequencing for the implementer wave

Touch list (everything else is reuse):

- `arch/common/dtr.c` — `kernel_infer_count` increments (3 sites:
  `train_forward`, `run_embed_seq`, plus `ai_job.c:mlp_forward`); declare in
  `arch/common/include/dtr.h`. NO other changes to the math files.
- `arch/x86/include/p_syscall.h` + `arch/x86/syscall.c` —
  `SYS_DTR_WEIGHTS_GET 0x213` (+ `SYS_MIND_NOTE 0x214` if CDN-7a).
- `userland/x86/ushim.c` (new) + `userland/x86/Makefile` (core_mind rule).
- `samples/12_ring3/03_core_mind/core_mind.c` (new — `_start`/argv/modes).
- `arch/x86/shell.c` — `ring3 mind` clauses inside `cmd_ring3`.
- `boot/x86/Makefile` — one `mcopy` line.
- `.github/workflows/ci.yml` — III.6 items 1-3.

The auditor (separate agent) re-derives the M1-M8 formula line-by-line,
re-runs from a cold boot, and additionally attempts falsification 3 by
hand-editing the shim to route through `SYS_INFER` and confirming the gate
goes red before the fix is reverted.

After Wave C, the remaining relocation order (the directive's full scope):
1. async infer split `0x240/0x241` (CDN-5) so a ring-3 mind can overlap
   compute and waiting; 2. ring-3 retrieval + weights WRITE-back at
   consolidation boundaries (CDN-2's batch path) — unlocks `lm`/`dmn` math
   in ring-3 with the DMN *scheduling* stub staying ring-0 (CDN-1a);
3. per-module address spaces (CDN-4b) + migrate `infer_d.elf` (CDN-9);
4. `gl_merge`/training relocation (needs the FPU fix from III.5 first);
5. the aarch64 EL0 mirror (II.5).
