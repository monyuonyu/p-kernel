# MC-2 — bare-metal constrained-SMP matmul workers: implementation plan (cert-first, the ②-enabling bringup)

**Status: DESIGN PLAN** by an automated design-harden on trunk `96a4411e` (`96a4411e MC-1: size-gated parallel matmul on the teacher forward + measured speedup`). Read-only on code; no implementation in this wave. Awaiting commander review + a separate impl→audit cycle.

mk_pino's real goal: **a NATIVE (bare-metal) node uses ALL its cores for the heavy matmul** — wake the parked aarch64 secondaries as deterministic matmul workers behind the SHIPPED `pk_parallel_rows` seam — WITHOUT making the T-Kernel scheduler SMP and WITHOUT breaking byte-identity ("one mind, one math", wave-49). The north star remains **② full SMP**; MC-2 is the deliberate **bringup foundation** ② reuses.

The boot/dispatcher path is the highest-risk class in this repo. This plan leads with the falsifiable cert and the safety boundary, and is deliberately conservative.

---

## 0. Read this first: the honest elephant (does bare metal even have a big matmul to benefit?)

**No — not today.** This must be said plainly before anything else, because it reframes what MC-2 *is*.

- The LLM "身体" tier (`quant.c`, `forward.c`, `pk_parallel.c`, the teacher's big matmuls) is **HOSTED-ONLY**. On bare-metal aarch64 it is **not linked**: `pk_parallel.c` appears only in `boot/linux/Makefile:249` (`LLM_C_SRCS = … pk_parallel.c …`), never in `boot/aarch64/Makefile`. The teacher's 49152-row output head and 1536-row ffn matmuls — the real MC-1 beneficiaries — **do not run on bare metal at all.**
- What bare-metal aarch64 *does* compile is the R3/MoE/dtr math: `boot/aarch64/Makefile` `COMMON_C_SRCS` includes `dtr.c`, `moe.c`, `r3_incontext.c`, `dtr_train.c`, `dmn.c`. The single matmul choke point there is `dt_linear` (`arch/common/dtr.c:148-157`):

  ```c
  /* y[M] = W[M×N] · x[N] + b[M] */
  void dt_linear(const float *W, const float *b,
                 const float *x, float *y, INT M, INT N) {
      for (INT m = 0; m < M; m++) {
          float s = b ? b[m] : 0.0f;
          for (INT n = 0; n < N; n++) s += W[m * N + n] * x[n];
          y[m] = s;
      }
  }
  ```

  This is the *exact* output-row shape MC-0/§1 partitions by: outer `m` (output row), self-contained inner `n` (contraction). **But it is TINY** — R3 today is `R_DM≈48`, `R_VALV=64` (`r3_incontext.c:81-88`), so the hottest `dt_linear` is ~48×48 ≈ 2304 MACs — three orders of magnitude *below* the shipped 524288-MAC gate (`pk_parallel.h:65`). At that size the dispatch/join cost would make parallel **slower** (plan §2.2, §5). And `dt_linear` does **not** call `pk_parallel_rows` today (confirmed: zero `pk_parallel` references in `dtr.c`/`moe.c`/`r3_incontext.c`).

**Therefore MC-2's value is NOT an immediate speedup. MC-2 is the SMP-bringup foundation for ②**, with the matmul cert proving the *deterministic-worker mechanism* on whatever matmul can run bare-metal — which, today, means a **synthetic boot self-test matmul** sized to exceed the gate (the cert vehicle), plus the real `dt_linear` shape kept byte-identical so that when SS-7 grows R3 past the gate, the mechanism is already proven. Do not let "native multicore" read to mk_pino as "the baby gets faster today." It does not. What it gets is: **the parked cores wake, run a deterministic tile, and the kernel survives it** — the literal first step of ② full SMP.

---

## 1. Ground truth: what a bare-metal aarch64 secondary needs (and what it does NOT)

### 1.1 The secondaries are parked, and there is no release mechanism today

`arch/aarch64/start.S:40-43` silences every non-primary core at reset:

```asm
    /* Silence all secondary cores immediately */
    mrs x0, mpidr_el1
    and x0, x0, #0xFF           /* extract CPU ID (Aff0) */
    cbnz x0, _secondary_park
```

and `start.S:144-146` is the park itself — the WAKE target:

```asm
_secondary_park:
    wfe
    b _secondary_park
```

There is **no per-CPU release address, no spin-table, no PSCI CPU_ON call** anywhere in the tree today — a secondary that wakes from `wfe` simply re-enters the park. MC-2 must add the release path. `_secondary_park` runs at the **reset EL (EL2 on both QEMU virt and RPi3 — `start.S:5-6`)** with **MMU off, caches off, no stack, no vectors** — the secondary has done *none* of the primary's EL2→EL1 setup (`start.S:75-134`).

### 1.2 MMU is OFF on bare metal — the single biggest simplification

The primary **never enables the MMU**. `start.S:80-83` sets a minimal SCTLR_EL1 with the MMU bit (bit 0) clear; `start.S:117-122` later sets only the data-cache (bit 2) and I-cache (bit 12) bits — **never bit 0 (M)**. Confirmed by `arch/aarch64/include/virtio_blk.h:16-17`: *"MMU is disabled at EL1 … so VA==PA."*

Consequence: **there are no page tables to share.** The textbook "secondaries need the SAME page tables for coherency" item is *moot here* — VA==PA on every core, all RAM identity-mapped. A secondary needs only the SAME minimal SCTLR (caches + FP) the primary set, not a TTBR handshake. This removes the classic SMP page-table-coherency minefield entirely. **Pin this honestly: MMU-off makes MC-2 materially safer than a textbook SMP bringup.** The cost is the OTHER coherency concern — §1.3.

### 1.3 Cache coherency: caches ON, but SMP coherency (CPUECTLR.SMPEN) is NOT set anywhere

`start.S:117-122` enables D-cache and I-cache on the primary. But there is **no write to `CPUECTLR_EL1.SMPEN` (S3_1_C15_C2_1, bit 6)** anywhere in the tree (confirmed: zero `cpuectlr`/`smpen`/`s3_1_c15` matches in `arch/aarch64/`). On a Cortex-A53 (`QEMU_FLAGS = -cpu cortex-a53`, `boot/aarch64/Makefile:329`), **SMPEN gates participation in the inner-shareable coherency domain**: with caches on but SMPEN off, a core's cacheable accesses are NOT guaranteed coherent with other cores. On real RPi3 this is the difference between "shared matmul buffers work" and "the primary reads stale cache lines for the workers' output = silent garbage = split mind." (QEMU TCG models memory coherently and may *mask* this — §8 risk; the canonical QEMU-vs-real-hardware trap.)

**MC-2 must set `CPUECTLR_EL1.SMPEN=1` on the primary AND on every secondary before it touches a shared buffer with caches enabled.** Small, well-known sequence, but load-bearing; belongs in the bringup, asserted by the cert's falsifier (§4).

### 1.4 GIC has no SGI/IPI plumbing today; PSCI-via-HVC exists on QEMU virt

- The GIC is initialized minimal (`tkdev_init.c:93-109`): distributor enable, the timer PPI (id 30), CPU-interface enable, priority mask. There is **no SGI (IPI) send path** and no `GICD_SGIR` usage anywhere. MC-2's worker wake/join uses **`sev`/`wfe` + shared-memory flags, NOT IPIs** — so the absent SGI plumbing is *not* on MC-2's critical path. (IPIs are an ② concern; §5.)
- **PSCI via HVC already works on QEMU virt**: `arch_reboot.c:4-5` documents *"QEMU exposes PSCI through HVC by default (psci-conduit=hvc)"* and `arch_reboot.c:45-50` already issues `PSCI_SYSTEM_RESET` via `hvc #0`. So **`PSCI CPU_ON` (function id `0xC4000003`, SMC64) via `hvc #0` is the QEMU virt secondary-bringup conduit** — no firmware work needed.
- **RPi3 has NO PSCI** (`arch_reboot.c:6` — *"RPi 3: no PSCI — BCM2837 boot has no secure firmware"*). On RPi3 secondaries are released by the **spin-table**: firmware parks each secondary spinning on a per-CPU mailbox in low memory (BCM2837 release addresses `0xE0`/`0xE8`/`0xF0` for cores 1/2/3); the primary writes the worker entry there and `sev`s. MC-2.2 (RPi3) uses this; MC-2.0/2.1 (QEMU) use PSCI CPU_ON.

### 1.5 The primary dispatcher idles on bare `wfe` — a subtle `sev` interaction

The T-Kernel dispatcher idles at `cpu_support.S:120-124`:

```asm
.Lidle:
    msr daifclr, #0x3
    wfe
    msr daifset, #0x3
    b .Ldispatch_loop
```

A bare `wfe` with no event-flag predicate. **A `sev` broadcast (to wake the matmul workers) will ALSO wake the primary out of its idle `wfe`** — harmless (it loops back to `.Ldispatch_loop` and re-idles) but worth stating: MC-2's worker `sev` is a global event and the primary's idle path already tolerates spurious wakeups by re-checking `knl_schedtsk`. **No change needed to the dispatcher.** This is exactly the safety margin: MC-2 never touches `.Ldispatch_loop`.

---

## 2. The deterministic invariant MC-2 must preserve (the crown — unchanged from MC-0)

The single non-negotiable invariant, identical to the shipped MC-0 seam (`pk_parallel.h:9-16`):

> **The parallel matmul produces a result BYTE-IDENTICAL to the serial loop, for ANY worker count (1, 2, 4) and ANY completion order — because only the outer output-index loop is split (never the contraction), each `y[i]` is written by exactly one worker (no shared accumulator), and the inner `acc += …` left-fold order is the serial order, verbatim.**

MC-2 changes only **who runs which output rows and on which physical core** — a pure scheduling decision over independent stores. It cannot change a single bit, *provided* the cache-coherency setup (§1.3) guarantees the primary reads the workers' completed output bytes-exactly. That coherency guarantee is the new MC-2 obligation that did not exist on hosted (where pthreads share one coherent address space). **This is why §1.3 (SMPEN) and §3.3 (barriers) are the heart of MC-2 — they make byte-identity TRUE across separate physical cores.**

Partition arithmetic is reused verbatim: `pk_slice` (`pk_parallel.c:57-63`) gives slice `s` the rows `[s·q, …)` with the ragged remainder `(out % nw)` on the LAST slice. MC-2's bare-metal backend MUST use the identical partition function so the bits match the hosted golden and the serial path.

---

## 3. The design

### 3.1 The seam: a bare-metal backend behind the SAME `pk_parallel_rows`

MC-2 does **not** invent a new API. It provides a **second implementation** of the shipped contract (`pk_parallel.h:104`):

```c
void pk_parallel_rows(size_t out, pk_row_body body, void *ctx);
```

- Hosted (`pk_parallel.c`): pthread pool. **Unchanged.**
- Bare-metal aarch64 (new, e.g. `arch/aarch64/pk_parallel_smp.c`): the secondary-core tile workers. Compiled into `boot/aarch64/Makefile` (a new `ARCH_OBJS` entry), guarded so x86 bare metal (no AP-startup; §6) keeps the inline-serial fallback.

The matmul call site is unchanged: `dt_linear` (and any future bare-metal big matmul) calls `pk_parallel_rows_gated(M, N, body, ctx)` with `body` = the unmodified serial inner loop parameterized by `[i0,i1)`. Below the gate (today's 48×48 R3) it runs `body(ctx,0,out)` inline — **identical bits, zero overhead, no secondary touched** (`pk_parallel.c:225-241` gate logic, reused). So wiring the seam into `dt_linear` is *safe even before the secondaries exist*: it's a no-op until something crosses the 524288-MAC gate.

> **HONEST:** wiring `dt_linear` to the seam is optional for MC-2.0/2.1 and can be deferred. The cert (§4) drives a **synthetic gate-exceeding matmul in a boot self-test** so the mechanism is proven *without* needing a big real matmul. Wiring `dt_linear` is the trivial last step that pays off only when R3 grows (SS-7).

### 3.2 Per-CPU data block + worker entry (sketch)

A tiny static per-CPU block (BSS, flat-mapped, VA==PA), one slot per secondary plus slot 0 for the primary-as-worker-0:

```c
/* arch/aarch64/pk_parallel_smp.c — sketch, NOT final */
#define PK_SMP_MAX_CPUS 4            /* QEMU -smp 4 / RPi3 = 4 cores */

struct pk_cpu {
    unsigned long  stack_top;       /* this core's own stack (own region)  */
    unsigned long  cpu_id;          /* mpidr Aff0                          */
    volatile unsigned long woken;   /* set once core reaches worker loop   */
} g_cpu[PK_SMP_MAX_CPUS];

/* The ONE global work-queue + ONE lock + done-counter (see §3.3). */
struct pk_wq {
    volatile pk_row_body body;      /* current job (NULL = no job)         */
    volatile void       *ctx;
    volatile size_t      out;
    volatile int         nw;        /* slices this dispatch partitioned in */
    volatile unsigned long gen;     /* bumped once per dispatch            */
    volatile int         done;      /* slices that have finished           */
    unsigned int         lock;      /* the one spinlock (ldaxr/stlxr)      */
} g_wq;
```

Per-secondary stacks: the linker gives the primary one 64KB stack (`linker.ld:43-47`, `_stack_top`). MC-2 adds **N-1 more stack regions** in the linker script (e.g. `_stack_top_cpu1..3`, 16KB each — workers run a leaf matmul body, no deep call chain), each core's `stack_top` baked into `g_cpu[]`.

**Secondary worker entry** (the fixed deterministic tile-loop — NEVER the T-Kernel dispatcher; `.Ldispatch_loop` untouched and the secondary never reads `knl_ctxtsk`/`knl_schedtsk`):

```asm
/* arch/aarch64/start.S — NEW _secondary_worker, the CPU_ON / spin-table target */
_secondary_worker:
    /* x0 = this core's struct pk_cpu* (via PSCI context_id or mpidr lookup) */
    ldr  sp, [x0, #PK_CPU_STACK_TOP]
    /* Match the primary's EL1 config: drop EL2->EL1 if needed, SCTLR
     * (I+C, NOT M — MMU stays off), CPACR (FP), and CRUCIALLY SMPEN=1
     * for inner-shareable coherency (§1.3). Reuse the primary's _from_el2
     * sequence factored into a shared macro. */
    bl   _secondary_el1_setup       /* SCTLR caches, FP, SMPEN, vbar */
    bl   pk_smp_worker_loop         /* never returns */
1:  wfe
    b    1b
```

```c
/* the C worker loop — drains tiles, then wfe; NEVER touches the scheduler */
void pk_smp_worker_loop(void) {
    unsigned long seen = 0;
    int slot = (int)(current_mpidr_aff0());   /* 1..N-1 */
    g_cpu[slot].woken = 1;
    for (;;) {
        while (g_wq.gen == seen)                /* no new job */
            __asm__ volatile("wfe");            /* BLOCK — no busy-spin (§5) */
        seen = g_wq.gen;
        __asm__ volatile("dmb ld" ::: "memory");/* ACQUIRE: see published job */
        if (slot < g_wq.nw) {
            size_t i0, i1;
            pk_slice(g_wq.out, g_wq.nw, slot, &i0, &i1);  /* SAME partition */
            if (i1 > i0) ((pk_row_body)g_wq.body)(
                              (void *)g_wq.ctx, i0, i1);   /* serial body */
        }
        __asm__ volatile("dmb st" ::: "memory");/* RELEASE worker's stores */
        wq_signal_done();                        /* done++ under the lock, §3.3 */
    }
}
```

The worker runs the **UNMODIFIED serial inner loop** for its rows. Determinism is identical to hosted (§2).

### 3.3 The ONE lock + the aarch64 BARRIER discipline (where SMP correctness lives)

A single global work-queue, ONE spinlock (`ldaxr`/`stlxr` exclusive-monitor pair, with `wfe`/`sev` backoff so a contended waiter sleeps rather than spins), one done-counter. **This is the entire correctness surface of MC-2 — a missing barrier = shared-memory race = non-deterministic garbage = split mind.** Specified precisely:

**Dispatch (primary, holding worker-0's slice itself):**
```
1. acquire g_wq.lock                       (ldaxr/stlxr)
2. g_wq.body/ctx/out/nw = job; g_wq.done = 0
3. dsb ish                                 ; job fields land in inner-shareable
                                           ;   domain BEFORE gen is bumped
4. g_wq.gen++                              ; PUBLISH (release semantics)
5. release g_wq.lock                       (stlr on lock word)
6. sev                                     ; wake all workers from wfe
7. run pk_slice(out,nw,0) body() itself    ; primary = worker 0
8. while (g_wq.done < nw-1) wfe            ; JOIN, block not spin
9. dmb ld                                  ; ACQUIRE before reading any y[i]
```

**Worker (secondary):** as §3.2 — `wfe` until `gen` advances, `dmb ld` (acquire) to observe the published job, run its slice, `dmb st` (release) its output stores, then under the lock `g_wq.done++` and `sev` (wakes the primary's join `wfe`).

**Memory-ordering rationale (the load-bearing part):**

| Hazard | The barrier that prevents it |
|---|---|
| Worker sees new `gen` but stale `body`/`ctx`/`out` | primary `dsb ish` (step 3) BEFORE bumping `gen`; worker `dmb ld` (acquire) AFTER reading `gen` |
| Primary reads a worker's `y[i]` before its store is globally visible | worker `dmb st` (release) after its body; primary `dmb ld` (step 9) before reading output |
| Two cores race the `done` counter | increment under the `ldaxr/stlxr` lock; release is `stlr`, acquire is `ldaxr`+`dmb` |
| Caches not coherent across cores at all | **CPUECTLR_EL1.SMPEN=1 on every core (§1.3)** — without it `dsb ish`/`dmb` operate on a non-shared domain and the scheme is unsound on real hardware |

Use **inner-shareable (`ish`) domain** barriers (all cores are one cluster). `dsb ish` for the publish (drains the store buffer to point-of-coherency); `dmb ld`/`dmb st` (acquire/release) for per-access ordering. The `sev`/`wfe` is the *wake* mechanism; the **barriers are the correctness** — `sev` carries no data-ordering guarantee, so a `sev` without the preceding `dsb` is exactly the falsifier's bug (§4).

### 3.4 What carries over to ② full SMP (explicit)

| MC-2 artifact (file:line where the seed exists) | ② full-SMP reuse |
|---|---|
| Secondary bringup: release from `_secondary_park` (`start.S:144`) → `_secondary_worker`, EL1 setup, SMPEN, own stack | ② needs the identical bringup to make a secondary *schedulable* |
| Per-CPU data block `g_cpu[]` (stack, id, woken) (§3.2) | becomes per-CPU `knl_ctxtsk`/`knl_schedtsk` + run-queue |
| The ONE work-queue lock (`ldaxr/stlxr` + `stlr` discipline) (§3.3) | template for the task-table / run-queue locks ② must add |
| `pk_parallel_rows` fork-join + `wfe`-idle/`sev`-wake contract | the wake/idle-halt machinery a real scheduler needs (the dispatcher's `.Lidle` `wfe`, `cpu_support.S:122`, becomes per-CPU) |
| CPUECTLR.SMPEN + `ish`-barrier discipline | survives unchanged; ② tasks doing matmul still hit the same byte-identity cert |

**What MC-2 deliberately does NOT do (the safety boundary ② must add):**
- **No per-CPU run-queue.** The secondary runs ONE fixed tile-loop, never picks a task.
- **No task migration.** Workers never touch `knl_ctxtsk`/`knl_schedtsk`.
- **No cross-CPU `knl_ctxtsk` locking.** Single `knl_ctxtsk` stays primary-only, single-threaded; the dispatcher (`cpu_support.S:48-124`) is byte-for-byte untouched.
- **No IPIs (SGI).** Wake is `sev`/`wfe` + shared flags; the absent `GICD_SGIR` path (§1.4) is an ② lift.

② is exactly the work of turning the single `knl_ctxtsk` into per-CPU scheduling **on top of** this bringup. MC-2 stops one step short of the scheduler — that boundary IS the safety margin.

---

## 4. THE CERT (cert-first)

### 4.1 `[mc2-smp-equiv]` — the gate (byte-identity, secondaries ON)

**Claim:** on bare-metal aarch64 under QEMU virt, the parallel matmul with secondaries woken is **byte-identical** to the single-core serial loop, across worker-core counts `{1, 2, 4}` on `qemu-system-aarch64 -smp 4`, under `-O1 -ffp-contract=off`.

**Harness — a BOOT SELF-TEST** (the bare-metal kernel cannot run a host process; the test lives *in the kernel*, invoked from `main.c` like the existing `ARK_BAREMETAL_SMOKE` hook at `main.c:62-76`):

1. Build a synthetic, fixed-seed `W` and `x` of a **gate-exceeding** shape (e.g. `out=2048, in=512 = 1048576 MACs > 524288`) in static BSS — large enough to actually dispatch to secondaries, deterministic across runs.
2. Compute `y_serial` by forcing `nw=1` (inline body — the serial path).
3. For `nw ∈ {2, 4}` compute `y_par` via the bare-metal `pk_parallel_rows` (secondaries woken).
4. Assert `memcmp(y_serial, y_par, out*sizeof(float)) == 0` — **byte-for-byte, not allclose** — and an **FNV-1a hash** of the output buffer (reuse the `student_shell.c` hash idiom) equal across all `nw`.
5. Emit the verdict over PL011 UART (`main.c:34-42` `print()`), halt with a recognizable `MC2-EQUIV: PASS/FAIL` line the run-harness greps.

**FALSIFIABILITY (mandatory — prove the barrier discipline is load-bearing):** add a debug-only **racy** variant that DELIBERATELY omits the publish `dsb ish` (step 3) and/or the join `dmb ld` (step 9), OR runs SMPEN=0. Under `-smp 4` on real ordering this MUST produce a `memcmp` mismatch (stale/torn output). If the racy variant *passes*, the barrier discipline isn't actually load-bearing in the harness → the cert is rejected as vacuous. **Honest caveat (§8 risk):** QEMU TCG models memory strongly and may MASK a missing-barrier or missing-SMPEN race — so the falsifier may only reliably fail on **real RPi3 hardware** (MC-2.2). The plan states this: the QEMU `[mc2-smp-equiv]` PASS proves the *partition arithmetic and worker mechanism*; the falsifier's teeth on the *barrier* discipline are only fully proven `[live]` on hardware. This is the salty-bug lesson (the host can't see the device's bug) applied to memory ordering.

**Why equivalence is expected to PASS by construction:** output-partitioning never reorders any inner accumulation (§2); `y_par[i]` runs the identical float ops in the identical order as `y_serial[i]`. The only ways it differs are real bugs — overlapping partitions, a worker writing the wrong range, a missing barrier/SMPEN letting the primary read stale output, or a stray FMA from a mis-set build flag — exactly what the cert catches.

### 4.2 `[mc2-boot-survives]` — the kernel still boots + schedules with secondaries woken

**Claim:** waking the secondaries does not perturb the primary's T-Kernel scheduler.

- The kernel boots to the existing `[BOOT] Starting T-Kernel...` banner (`main.c:78`) and reaches `knl_t_kernel_main` (`main.c:79`) with the secondaries woken into the worker loop. The init task runs and the timer ticks (primary scheduler unaffected — `.Ldispatch_loop`/`.Lidle` untouched).
- **A worker fault does not wedge the primary.** A secondary that takes a fault (e.g. a deliberately bad tile) must NOT hang the primary's join indefinitely — the join must have a bounded fallback (a watchdog count, or the faulting core's slot never increments `done` → the join must time out and the primary recovers/reports, never spins forever). The cert deliberately faults one worker tile and asserts the primary still prints a verdict and keeps scheduling.

### 4.3 Is QEMU `-smp 4` byte-identity actually TESTABLE in this repo's harness? — HONEST answer

**The mechanism exists but the harness does NOT yet — and the current QEMU invocation is single-core.** Ground truth:
- `qemu-system-aarch64` IS installed (`/usr/bin/qemu-system-aarch64`).
- The current run target has **NO `-smp` flag**: `QEMU_FLAGS = -M virt -cpu cortex-a53 -m 256M …` (`boot/aarch64/Makefile:329`) → QEMU defaults to **1 CPU**. So *today the secondaries don't even exist in the VM.* MC-2.0 must add `-smp 4` to a new run target (e.g. `run-smp`).
- There is **no automated grep-the-UART pass/fail harness** for the aarch64 bare-metal kernel today; the existing targets (`run`, `run-smoke`, `run-debug`, `run-rpi3`) just stream serial to stdio for a human. The `ARK_BAREMETAL_SMOKE` hook (`main.c:62-76`) is the proven *pattern* — a boot-time self-test that prints `SMOKE RESULT: PASS/FAIL` and halts. MC-2 must add an analogous `MC2_SMP_SELFTEST` hook + a `tests/` script that runs `qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 4 … -kernel kernel.elf` with a timeout and greps the UART for `MC2-EQUIV: PASS`.

**So: yes, byte-identity under QEMU `-smp 4` is testable — but only after MC-2.0 BUILDS the harness (the `-smp` run target + the boot self-test + the grep script).** Until that exists the cert is paper. The first slice's primary deliverable is making the cert *real*, not green. This is the most important honesty in the plan: **the harness must be built before the claim is made.**

### 4.4 Marking
- MC-2.0 / MC-2.1: `[live]` on QEMU virt `-smp 4` (TCG — real instruction execution on emulated cores; observes the partition mechanism, may mask barrier races per §4.1).
- MC-2.2: `[live]` on RPi3 hardware — the ONLY place the barrier/SMPEN falsifier is fully load-bearing. Likely DEFERRED (needs physical hardware reachable).
- The byte-identity cert is **the gate**; any speedup figure is **honest reporting** and must never lower the equivalence bar. (No speedup is even claimed at MC-2 — there is no big bare-metal matmul; §0.)

---

## 5. Idle / overhead / when-NOT discipline (carried from MC-0, native to the ISA)

1. **Workers BLOCK on `wfe`, never busy-spin** when the queue is empty (§3.2 loop). Mirrors both the hosted condvar futex-sleep (`pk_parallel.c:76-77`) and the primary dispatcher's own `wfe`-when-idle (`cpu_support.S:122`). Between matmuls the secondaries draw ~zero power. A spinning pool would fight the timer/scheduler and burn an RPi3's power budget — explicitly forbidden, and asserted: after a quiescent gap the workers must be in `wfe` (observable as zero forward progress on a wake-counter analogous to `pk_parallel_wake_count`, `pk_parallel.c:140-143`).
2. **The gate keeps tiny matmuls serial.** Today's only bare-metal matmul (`dt_linear`, 48×48) is below the 524288-MAC gate (`pk_parallel.h:65`), so `pk_parallel_rows_gated` runs it INLINE on the primary — no secondary touched, zero dispatch cost, byte-identical. The secondaries stay asleep for the entire shipped baby workload. (The §0 elephant restated as a safety property: MC-2 is inert for the current mind.)
3. **The `sev` is global.** It wakes the workers AND spuriously wakes the primary's idle `wfe` (§1.5) — harmless, the dispatcher re-checks and re-idles. No dispatcher change.

---

## 6. DEFERRED / OUT-OF-SCOPE

- **x86 bare-metal multicore — DEFERRED, a separate bigger lift.** No AP-startup (INIT-SIPI-SIPI) on x86 bare metal today; MC-2 is **aarch64 ONLY**. x86 keeps the inline-serial fallback.
- **Wiring `dt_linear`/`moe.c`/`r3_incontext.c` to the seam** is optional for MC-2.0/2.1 (the cert uses a synthetic self-test matmul). Defer until a real bare-metal matmul crosses the gate (SS-7 growing R3).
- **Any change to the T-Kernel scheduler** (`knl_ctxtsk`/`knl_schedtsk`, `cpu_support.S` dispatch). The kernel stays uniprocessor. (That is ②.)
- **Splitting the contraction** dim across workers (would reassociate; breaks byte-identity). Only output-row partitioning ships.
- **GIC SGI/IPI plumbing.** MC-2 wakes via `sev`/`wfe`; IPIs are an ② concern.

---

## 7. Sequencing — small falsifiable waves

**MC-2.0 — bringup + ONE secondary running a trivial deterministic tile; `[mc2-boot-survives]` on QEMU. (THE SMALLEST REAL SLICE.)**
- Add a `-smp 4` run target (`run-smp`) to `boot/aarch64/Makefile` (§4.3).
- Add `_secondary_worker` + `_secondary_el1_setup` (SCTLR caches+FP, **SMPEN=1**, vbar; MMU stays off) to `start.S`, factored from the existing `_from_el2` path.
- Release exactly ONE secondary via **PSCI CPU_ON (`0xC4000003`) over `hvc`** (reuse the `arch_reboot.c:50` HVC idiom), give it its own stack (new linker region), enter the worker loop.
- Run ONE trivial fixed tile (e.g. fill a known buffer) and signal done via the one-lock done-counter; primary joins.
- **Cert `[mc2-boot-survives]`:** kernel boots to `[BOOT] Starting T-Kernel...`, the secondary reaches the worker loop (`g_cpu[1].woken==1`), the primary scheduler still ticks, and a deliberately faulting tile does not wedge the primary's join. *Proves the parked core wakes and the kernel survives it — the literal first step of ②.*

**MC-2.1 — the work-queue + N secondaries + `[mc2-smp-equiv]` byte-identity on QEMU `-smp 4`.**
- Generalize to N-1 secondaries, the full §3.3 lock + `ish`-barrier discipline, the §3.2 worker loop with `pk_slice`.
- Add the boot self-test matmul (gate-exceeding synthetic shape) + the FNV hash + the UART verdict + the `tests/` grep harness (§4.3).
- **Cert `[mc2-smp-equiv]`:** `memcmp==0` + hash equal across `nw ∈ {1,2,4}`; the racy/`SMPEN=0` **falsifier MUST mismatch** (with the honest QEMU-may-mask caveat, §4.1). Idle assertion: workers in `wfe` between matmuls.

**MC-2.2 — RPi3 hardware `[live]` (likely DEFERRED).**
- Spin-table release (BCM2837 mailboxes, not PSCI — `arch_reboot.c:6`); the SAME worker loop + barriers.
- The ONLY place the barrier/SMPEN falsifier is fully load-bearing (real cache coherency). Run when physical hardware is reachable; netboot tooling (`make tftp`, `make run-rpi3`, `boot/aarch64/Makefile:363-366`) is the on-ramp.

**Phase C / ② (north star, separate future wave).** Per-CPU run-queue + cross-CPU scheduler **on top of** MC-2's bringup + per-CPU data + lock discipline + SGI/IPI plumbing. Out of scope here.

---

## 8. OPEN RISKS (honest — bare-metal SMP bringup is the named biggest risk in this repo)

1. **Cache-coherency / barrier correctness = the race minefield (the #1 risk).** A missing `dsb ish`/`dmb`, or an unset `CPUECTLR.SMPEN` (§1.3 — *nothing* sets it today), is a silent shared-memory race → non-deterministic garbage → split mind. The §3.3 barrier discipline closes this; the §4.1 falsifier proves it load-bearing. Conservatism: review barrier placement line-by-line in audit; do not credit a PASS the falsifier can't also FAIL.
2. **QEMU-vs-real-hardware coherency (the salty-bug shape for memory ordering).** QEMU TCG models memory strongly and may **mask** a missing-barrier or SMPEN=0 race that *would* corrupt on a real RPi3. The QEMU `[mc2-smp-equiv]` PASS proves the mechanism but NOT the barrier teeth; the falsifier is only fully load-bearing `[live]` on hardware (MC-2.2). State this in every cert claim. A green QEMU run does NOT mean "barriers verified."
3. **The boot/dispatcher path is the highest-risk class in this repo.** MC-2 adds to `start.S` (the reset path) and a CPU_ON HVC. A bug here is a silent boot hang (the friendly-fail discipline of `main.c:52-60`'s static asserts is the model). Mitigation: MC-2.0 keeps it to ONE secondary and a trivial tile; the dispatcher (`cpu_support.S`) is byte-for-byte untouched; the primary boot path through `_el1_entry` (`start.S:96-134`) is unchanged.
4. **The tiny-matmul reality (§0).** MC-2 delivers **no speedup to the shipped baby** — the only bare-metal matmul is below the gate. Its value is the ②-bringup foundation + the deterministic-worker mechanism, proven on a synthetic self-test matmul. Do not oversell "native multicore" as a baby-speedup.
5. **PSCI CPU_ON conduit assumptions.** QEMU virt exposes PSCI via HVC (`arch_reboot.c:4-5`) — assumed but must be confirmed at the function-id level (`0xC4000003`, SMC64) and that QEMU's default psci-conduit is HVC for `-M virt` at this QEMU version. RPi3 has NO PSCI (spin-table only). EL mismatch (secondary released at EL2 vs CPU_ON landing EL) must be handled in `_secondary_el1_setup` exactly as the primary's `_from_el2`.
6. **SGI/IPI absence.** MC-2 sidesteps it with `sev`/`wfe`, but ② will need it and the GIC has no SGI path today (§1.4). Flagged for ②.
7. **Whether the LLM tier ever runs bare metal at all.** Today it does not (§0). If it never does, MC-2's *matmul* value stays synthetic-cert-only; its *bringup* value (for ②) is the real payoff regardless.

---

### Appendix — grounding (file:line)

- Parked secondaries / wake target: `arch/aarch64/start.S:40-43` (silence), `:144-146` (`_secondary_park: wfe`). Reset EL is EL2: `start.S:5-6`.
- MMU OFF on bare metal (no TTBR, VA==PA): `start.S:80-83` (SCTLR min, M-bit clear), `:117-122` (only C+I bits set, never bit 0); confirmed `arch/aarch64/include/virtio_blk.h:16-17`.
- SMPEN never set (the coherency gap): zero `cpuectlr`/`smpen`/`s3_1_c15` matches in `arch/aarch64/`.
- PSCI via HVC on QEMU virt (the CPU_ON conduit): `arch/aarch64/arch_reboot.c:4-5,45-50`. RPi3 no PSCI: `arch_reboot.c:6`.
- GIC minimal init, no SGI path: `arch/aarch64/tkdev_init.c:93-109`; GICD/GICC bases `arch/aarch64/include/tkdev_conf.h:25-29`.
- Primary dispatcher + bare-`wfe` idle (UNTOUCHED by MC-2): `arch/aarch64/cpu_support.S:48-124`, `.Lidle` `:120-124`.
- The shipped seam (the contract MC-2 re-implements): `arch/common/llm/pk_parallel.h:104` (`pk_parallel_rows`), `:76` (`pk_parallel_rows_gated`), `:65` (`PK_PARALLEL_MIN_MACS 524288`), `:9-16` (the invariant); partition `arch/common/llm/pk_parallel.c:57-63` (`pk_slice`, ragged remainder on LAST slice), `:225-241` (gate logic), `:140-143` (wake counter).
- Hosted-only tiering (pk_parallel NOT on bare metal): `boot/linux/Makefile:249` (only place `pk_parallel.c` is built); `arch/common/llm/pk_parallel.h:18-24`.
- The bare-metal matmul that DOES exist (below gate): `arch/common/dtr.c:148-157` (`dt_linear`); R3 dims `arch/common/r3_incontext.c:81-88` (R_DM≈48, R_VALV=64). Bare-metal COMMON sources incl. `dtr.c`/`moe.c`/`r3_incontext.c`: `boot/aarch64/Makefile` `COMMON_C_SRCS`.
- The boot self-test PATTERN for the cert: `boot/aarch64/main.c:62-76` (`ARK_BAREMETAL_SMOKE` hook, UART `print()` verdict `:34-42`, halt), `main.c:52-60` (friendly-fail static asserts).
- QEMU invocation is single-core today (no `-smp`): `boot/aarch64/Makefile:329` (`QEMU_FLAGS`); RPi3 target `:363-366`; `qemu-system-aarch64` at `/usr/bin/`.
- MC-0/MC-1 cert discipline (the falsifier model): `tests/llm/run_mc0.sh`, `tests/llm/mc0_test.c`, `tests/llm/run_mc1.sh` (uses `PKERNEL_MATMUL_THREADS` env in-process, NOT QEMU `-smp`).
- "One mind, one math" (`-ffp-contract=off`, byte-identity crown): per MEMORY.md wave-49; `arch/common/llm/student.c:696` (ASCENDING-order canonical reduction).

---

**Note:** the design-harden agent could not write this file (Write was permission-denied in its sandbox); the commander persisted it verbatim from the agent's returned plan. Awaiting commander review + a separate impl→audit cycle (MC-2.0 first).
