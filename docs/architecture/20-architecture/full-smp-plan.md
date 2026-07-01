# ② full SMP — making the T-Kernel scheduler symmetric-multiprocessing without splitting the mind: implementation plan (cert-first, multi-wave)

> **現在地（2026-07-01・doc-hygiene 追記／本文は 年輪 として保存）:** **②.0–②.2 は SHIPPED**（`arch/aarch64/smp.c`＋`smp_async.c`/`smp_secwait.c`/`smp_onemind.c`/`mc2_smp.c`、cert `tests/aarch64/run_smp0..5.sh`）。本文の残る生きたロードマップは **②.3** のみ。旧 SMP 個別プランは `archive/` へ移設済。

**Status: DESIGN PLAN** by an automated design-harden on trunk `432cf337`. Read-only on code; no implementation in this wave. This plan makes ② **READY and de-risked** — it does **not** start it. ②'s implementation is a 本丸-level decision **awaiting mk_pino's go-ahead** + per-wave **separate impl→audit cycles** (implementer ≠ auditor ≠ commander).

mk_pino's stated END GOAL: *"最終的に2番にしたい"* — turn the T-Kernel **UNIPROCESSOR** scheduler into a real **symmetric-multiprocessing** kernel where N cores each run schedulable T-Kernel tasks, WITHOUT breaking the byte-identity crown ("one mind, one math", wave-49) or the determinism the distributed mind depends on. This is the **highest-risk change in the repo** (it took Linux ~10 years). The disciplined claim of this plan: p-kernel can do a *small-wave, cert-first* version far faster **because the mind's heavy-math determinism is already solved** by MC-2's output-partition + the canonical reduction orders — so the determinism threat reduces to a **kernel-state race** problem, not a mind-math problem. That distinction is the entire plan; it leads §1.

This plan **builds directly on MC-2 / MC-2.1**, which already shipped + audited the bringup substrate ② reuses (secondary-core wake via PSCI CPU_ON, per-CPU `g_cpu[]`, the `ldaxr/stlxr` lock, the `dsb ish`/`dmb` barrier discipline, SMPEN, per-CPU stacks). See `docs/architecture/mc2-baremetal-smp-plan.md` §3.4 and `docs/architecture/mc2-1-ncore-equiv-plan.md` §7 for exactly what MC-2 hands ②. ② **adds** what MC-2 deliberately stopped short of: per-CPU run-queues, cross-CPU `knl_ctxtsk` locking, real kernel locks (replacing DI/EI), and IPIs (the GIC has **no SGI path today**).

---

## 1. The determinism threat model — LEAD WITH THIS (what SMP threatens that MC-2 did NOT)

MC-2 proved one thing precisely: the heavy matmul is **byte-identical regardless of which core runs which output row** (`pk_parallel.h:9-16`; cert `[mc2-smp-equiv]`). That is a statement about **independent stores under output-partitioning** — a pure-arithmetic property. MC-2 never made the **scheduler** concurrent: the secondary "runs ONE fixed tile-loop, never picks a task… never touches `knl_ctxtsk`/`knl_schedtsk`" (`mc2-baremetal-smp-plan.md` §3.4). The single `knl_ctxtsk`/`knl_schedtsk` stayed primary-only, single-threaded; `cpu_support.S:48-124` (the dispatcher) was byte-for-byte untouched.

② changes exactly that. It makes the **scheduler** run on N cores at once. The threat is therefore **NOT the mind's math** (already deterministic) but **shared mutable KERNEL state now raced by N CPUs**, plus a smaller set of timing/ordering effects. We classify every risk into one of two categories, because they have different cures:

- **CORRECTNESS race** — two CPUs mutate shared state with no real lock → corruption (wrong answer, crash, hang). Cure: **a real lock** (or per-CPU-ization).
- **DETERMINISM race** — no corruption, but the *result* depends on the interleaving/timing of events → two runs could produce different bits. Cure: **serialize or make the combination commutative/canonical** (or prove it already is).

**The crown is byte-identity of the MIND'S OUTPUT, not of kernel scheduling traces.** Kernel scheduling order is *allowed* to differ run-to-run (it already does on the hosted port under SIGALRM jitter); what must NOT differ is the mind's forward / consolidation output. So for each risk we also state whether the one-mind crown is **actually at risk** or is **already insulated** by MC-2's math determinism + the canonical orders.

### 1.1 CORRECTNESS races — shared mutable kernel state raced by N CPUs (the real bulk of ②)

These are the textbook SMP problem. On a uniprocessor they were free, because **"disable interrupts" IS the lock**: a critical section under `DI()`/`EI()` cannot be interrupted on the only CPU, so no other context can race it. Under SMP, `DI()` masks only the **local** CPU's interrupts — a second CPU in the kernel races right through it.

The proof that this is the dominant ② problem, grounded:

| Shared state | Today's "lock" (uniprocessor) | file:line | Under SMP |
|---|---|---|---|
| **Ready queue** `knl_ready_queue` (bitmap + per-prio queues + `top_priority` + `klocktsk`) | `BEGIN/END_CRITICAL_SECTION` = `disint()`/`enaint()` | `ready_queue.h:44-50`; macros `arch/aarch64/include/cpu_status.h:17-24` | N CPUs inserting/deleting → bitmap/`top_priority` invariant corrupts → `knl_tstdlib_bitsearch1` infinite loop (THIS EXACT BUG already observed on the hosted port — §1.4) |
| **Scheduler globals** `knl_ctxtsk`, `knl_schedtsk`, `knl_dispatch_disabled` | single global, dispatcher-owned | def `task.c:62-63,55`; reschedule `task.h:240-249` | one running task / one next task is **the uniprocessor assumption itself** — must become per-CPU (§3) |
| **The allocator** `knl_Imalloc`/`knl_Ifree` (FreeQue) | **`DI(imask)` — comment: "Exclusive control by interrupt disable"** | `memory.c:275`, `:364` | N CPUs racing the free list → heap corruption → split mind via corrupted weight buffers |
| **Object tables** (sem/mtx/evtflag/mbox/msgbuf/mempool/mempfix wait queues) | `BEGIN_CRITICAL_SECTION` / `BEGIN_DISABLE_INTERRUPT` | eventflag.c, mailbox.c, mempool.c, mempfix.c, messagebuf.c, mutex.c (all `BEGIN_*`) | every wait-queue insert/remove is a cross-CPU race |
| **Kernel object lock** `knl_LockOBJ`/`knl_UnlockOBJ` + `RDYQUE.klocktsk` | `BEGIN_CRITICAL_SECTION` + the *"Multiple READY tasks with kernel lock do not exist at the same time"* invariant | `klock.c:55,98`; invariant comment `ready_queue.h:38` | **this invariant is literally a uniprocessor assumption** — only one task can hold the big kernel lock because only one runs; ② must make `klocktsk` a real lock or per-CPU |
| **`fastlock`/`fastmlock`** counters/bits | `DI(imask)`/`EI(imask)` around `++`/`--`/`BTS`/`BR` | `fastlock.c:61-64,84-87`; `fastmlock.c:65-68,80-83,100-103,119-121` | non-atomic RMW under local-only mask → lost updates, double-acquire |
| **Timer queue** `knl_timer_queue` + `knl_current_time` | `BEGIN_CRITICAL_SECTION` in `knl_timer_handler` | `timer.c:183-228` | if every CPU has its own tick (§5), N handlers race the one timer queue + the 64-bit `knl_current_time` |

**Verdict for §1.1: all CORRECTNESS races. None of these touch the mind's *math* — but a corrupted ready queue or heap absolutely produces a wrong/dead mind. The crown is at risk INDIRECTLY (via corruption), not via arithmetic.** The cure is mechanical and well-understood: a lock around each. The **Big Kernel Lock** (§2) closes ALL of these in one stroke as the safe first wave, because it makes "only one CPU is in the kernel at a time" true again — restoring the exact uniprocessor invariant for kernel code while letting tasks run concurrently in compute.

### 1.2 The DI/EI critical sections that were free on a uniprocessor (the mechanism, restated)

`disint()` on aarch64 is `mrs daif; msr daifset, #0x3` (`arch/aarch64/include/cpu_insn.h:18-27`) — it sets the **local** core's DAIF I+F bits. `BEGIN_CRITICAL_SECTION` wraps `disint()` and `END_CRITICAL_SECTION` checks `knl_ctxtsk != knl_schedtsk` and calls `knl_dispatch()` on exit (`arch/aarch64/include/cpu_status.h:17-24`). `knl_dispatch_request()` is a **no-op** (`include/kernel/tkernel/cpu_status.h:104`, comment "dispatch_request is no-op; actual dispatch happens at END_CRITICAL_SECTION"); the dispatch is *deferred to the critical-section exit*. This is the uniprocessor critical-section primitive in full: **mask local IRQs → mutate shared state → maybe dispatch → unmask.** Every one of the §1.1 rows is one of these. Under SMP each becomes either a real lock or a race; §2 maps the conversion.

### 1.3 DETERMINISM races — mind-state whose RESULT could depend on interleaving/timing

This is the category the crown actually cares about. The determinism probe (grounded below) found the mind's computation is **already insulated** — it depends on math inputs and canonical orders, not on task scheduling order:

| Mind-state | Append/merge order driver | file:line | Category & crown verdict |
|---|---|---|---|
| **DMN engram ring** (sleep consolidation) | engrams appended by a **deterministic stride** (`step = LM_NTR / B_RING`), not by arrival/scheduling order; weighted slots by **largest-remainder with a deterministic tiebreaker** ("ties → lowest class index") | `lm_consolidate.c:296-306`, `:322-327` | **(A) math-only.** SMP cannot change which engram lands in which slot. **Crown NOT at risk.** |
| **DMN replay order** | fixed nested loop `for tp … for j …` over tasks then indices | `lm_consolidate.c:402-407` | **(A) math-only.** Canonical. **Crown NOT at risk.** |
| **DMN tick cadence** | wall-clock `tk_dly_tsk(DMN_PULSE_MS)` + a deterministic pulse counter | `dmn.c:468-494` (pulse `:469`, idle_for `:472`) | timing-dependent *cadence* but the **consolidation INPUT set is the ring (deterministic)**; a sleep firing earlier/later replays the **same** engrams. **Crown NOT at risk.** |
| **`gl_merge` / `rw[]` weight merge** | flat *set* of models, `for k: accumulate; scale 1/count` — algebraically order-independent SUM; the `[g22-no-central]` test proves forward-vs-reverse differ only by ~1e-6 IEEE rounding | `gossip_learn.c:69-75`, order-indep comment `:114-115`, test `:481-512` | **(A) math-only, MODULO IEEE float-add non-associativity.** SMP changes neither *which* models merge nor *count*. The ~1e-6 rounding is the **same honest property merging already has on the hosted port** — not new to ②. **Crown effectively NOT at risk** (see §1.3.1). |
| **R3 in-context** `r_forward` | pure math over `rw[]` + input tokens; no global reads, no RNG | `r3_incontext.c:192-258` | **(A) math-only.** **Crown NOT at risk.** |
| **R3 fact queue** `s_round` | trains **oldest-pending by `seq`** (FIFO); minibatch cycled `st % m` | `r3_incontext.c:1419-1420`, `:1437-1447` | `seq` is assigned at arrival; training order is **canonical (oldest-first)**, not scheduling-order. SMP could change *arrival interleaving* of two facts (§1.3.2). |

#### 1.3.1 The one honest asterisk: IEEE float-add order in `gl_merge`

`gl_merge` (`gossip_learn.c:69-75`) sums models in **array order `models[0..count)`**. Float addition is not associative, so if SMP ever changed the *order the model pointers are assembled into that array*, the last ~1e-6 bits could differ. **But:** (a) the array is assembled deterministically from the node set, not from task-scheduling order, and (b) this is **already true on the hosted port** — wave-41/42 measured and accepted it (MEMORY.md: "naive weight-averaging … is LOSSY … union-replay recovers BOTH"; the merge's order-independence-modulo-rounding is the *shipped* property). **②'s obligation is narrow: do not let SMP reorder the merge INPUT array.** If the input assembly stays canonical (sorted by node-id / deterministic walk, which it already is), `gl_merge` is byte-stable. This is a **DETERMINISM race that is already cured by the existing canonical input order** — ② must merely *not regress it*, and the `[smp-one-mind]` cert (§6c) is exactly the guard.

#### 1.3.2 Fact arrival interleaving (the only genuinely new determinism surface)

Two facts arriving "at the same time" from the network: under cooperative scheduling, message-handler tasks ran one-at-a-time so `seq` assignment was effectively serialized by the single CPU. Under SMP, two CPUs could assign `seq` to two facts concurrently. **Classification: a CORRECTNESS race on the `seq` counter (must be atomic/locked), NOT a mind-determinism race on the result** — because once `seq` values are assigned, `s_round` trains them in canonical oldest-first order (`r3_incontext.c:1419`) regardless. The *which-fact-got-the-lower-seq* tie is a real-world non-determinism that **already exists** (network packet arrival order is non-deterministic on any node, uniprocessor or not — two phones can't agree on "same time"). So this is **not a crown regression**: the mind already tolerates network arrival jitter; SMP just moves the tie-break from "single-CPU handler order" to "lock-ordered `seq` assignment". The cure is a locked/atomic `seq` (folded into §2's object-table locking), and the existing FIFO canonicalization absorbs the rest.

### 1.4 The decisive precedent: the hosted port ALREADY hit ②'s race — and named it

This is the single most important grounding in the plan. `arch/linux/aarch64/preempt.c:199-211` documents, verbatim, the exact SMP corruption ② must prevent — discovered when the SIGALRM timer was delivered to the **wrong thread** so `knl_timer_handler_startup` ran **CONCURRENTLY with task code**:

> *"knl_timer_handler_startup then runs CONCURRENTLY with task code on the kernel thread — arch_irq_disabled_flag cannot protect across threads — and corrupts the ready/timer queues. Observed in multi-node so_node runs as (a) a survivor segfault and (b) a livelock: two nodes spinning forever at the same PC in knl_tstdlib_bitsearch1 under knl_ready_queue_delete, whose inner loop never terminates once the bitmap/top_priority invariant is broken."*

The fix there was to **pin the signal to the single kernel thread** (`SIGEV_THREAD_ID`, `preempt.c:216-225`) — i.e. to *restore the uniprocessor invariant*. ② cannot do that; ② must instead make the ready-queue access actually **mutually exclusive across CPUs**. **This proves three things:** (1) the race is real and reproducible, not theoretical; (2) the corruption mode is precisely a broken `bitmap`/`top_priority` invariant in `knl_ready_queue_delete` → `bitsearch1` livelock — the cert's falsifier should target exactly this; (3) `arch_irq_disabled_flag` (the hosted DI/EI, `preempt.c:39`) is **explicitly documented as NOT protecting across threads** — the hosted analogue of "DI masks only the local CPU." ② is, in essence, the disciplined generalization of the fix that hosted made by retreating.

### 1.5 Threat-model summary (one paragraph)

**SMP threatens kernel STATE (ready queue, allocator, object tables, scheduler globals, timer queue, fastlocks) with CORRECTNESS races — because the uniprocessor "DI/EI = lock" assumption collapses when a second CPU is in the kernel.** It threatens the mind's OUTPUT only **indirectly** (a corrupted heap/ready-queue kills the mind) — **NOT** arithmetically, because MC-2 made the matmul order-independent and the mind's append/merge/replay paths are already canonical (deterministic stride, FIFO-by-seq, order-independent SUM with a fixed tiebreaker; §1.3). The **one honest asterisk** is `gl_merge`'s IEEE float-add order, which is already a shipped, accepted ~1e-6 property and only needs ② to **not reorder the merge input array** (it already won't). Therefore: **the crown ("one mind, one math") is at risk via CORRUPTION, not via concurrency-induced reordering — and the Big Kernel Lock (§2) eliminates the corruption risk wholesale in the first wave by making the kernel single-threaded-internally again.** This is why ② is tractable in small waves where Linux's general-purpose SMP was not: p-kernel can ship a correct-but-coarse BKL kernel first and prove byte-identity, then refine locks only where measured contention demands it.

---

## 2. The locking architecture — Big Kernel Lock first, then staged finer locks

### 2.1 Wave-0 safety boundary: ONE Big Kernel Lock (BKL)

The simplest correct thing, and the SAFE first wave: **one giant lock around all kernel entry.** Only one CPU is inside the kernel at a time; tasks run concurrently in **userspace/compute** (the matmul, the mind's forward — exactly where MC-2 already proved concurrency is byte-safe), but every **kernel call serializes**. This preserves the uniprocessor invariants almost verbatim — the §1.1 races vanish because the kernel is once again single-threaded *internally*.

**Mechanism — map DI/EI → BKL, minimally invasive:**
- `disint()`/`enaint()` (`arch/aarch64/include/cpu_insn.h:18-35`) keep masking the **local** CPU's IRQs (still needed — a CPU must not be interrupted mid-critical-section), **and additionally** acquire/release the one global BKL spinlock.
- Concretely: redefine `BEGIN_CRITICAL_SECTION` / `BEGIN_DISABLE_INTERRUPT` (`arch/aarch64/include/cpu_status.h:17,30`) to, after `disint()`, **acquire `g_bkl`**; redefine the `END_*` to **release `g_bkl`** before `enaint()`. Because every kernel critical section already routes through these two macro pairs (verified: every subsystem in §1.1 uses `BEGIN_CRITICAL_SECTION`/`BEGIN_DISABLE_INTERRUPT`), wrapping the macros is a **single-point change that covers all of them**. The allocator's bare `DI(imask)` (`memory.c:275,364`) is the one exception that must be hand-converted to `BEGIN_DISABLE_INTERRUPT`/BKL (or acquire `g_bkl` directly) — enumerate it explicitly (the "cert must cover ALL paths" lesson: a lock that misses one bare-`DI` site silently misses that race).
- **Re-entrancy:** the BKL must be **recursive per-CPU** (a CPU already holding it — e.g. a timer IRQ nested inside a syscall already in the kernel — must not deadlock against itself). Track owner-CPU + depth; acquire only on the outermost entry. This is the one subtlety; the cert's `[smp-no-deadlock]` (§6d) guards it.

**Lock type (aarch64 bare metal):** a **ticket spinlock** or **MCS spinlock** built on the MC-2 `ldaxr/stlxr` + `stlr` discipline (`mc2_smp.c:110-129`, reused) with the `dsb ish`/`dmb` barrier discipline (`mc2_smp.c:229,159,175,258,268`). A ticket lock gives FIFO fairness (no starvation under contention) at the cost of one extra word; an MCS lock scales better at high core counts but is more code. **For ②.0 a ticket spinlock is the right first choice** (4 cores, BKL → low contention by construction since compute runs outside the lock). Waiters should `wfe`/`sev`-back-off (the MC-2 `sevl/wfe` idiom, `mc2_smp.c:110-129`) so a contended waiter sleeps rather than burns power.

**Lock type (hosted Linux port, "CPUs" are threads):** the hosted port has **no real DI/EI** — `arch_irq_disabled_flag` is a plain flag explicitly documented as not protecting across threads (`preempt.c:39`, `:199-211`). So the hosted BKL must be a **real `pthread_mutex_t`** (recursive, `PTHREAD_MUTEX_RECURSIVE`) or a futex. `disint()`/`enaint()` on hosted (`arch/linux/{aarch64,x86_64}/include/cpu_insn.h:20-35`) keep the flag for IRQ-deferral semantics **and** acquire/release the pthread BKL. (This is precisely the generalization of the `SIGEV_THREAD_ID` fix — instead of forcing one thread, allow N threads but serialize kernel entry under a real mutex.) Hosted-port SMP is a **separate lift** (§8) but the BKL design is identical in shape.

### 2.2 Staged path to finer locks (the perf wave — DEFER past ②.2)

Once `[smp-one-mind]` (§6c) proves the BKL kernel is byte-identical, *then and only then* split the BKL by measured contention, in this order (each its own cert + falsifier):
1. **Ready-queue lock** `g_rqlock` — the hottest (every dispatch + every wake). Map: the `BEGIN_CRITICAL_SECTION` regions in `task.c`/`task_manage.c`/`wait.c` that touch `knl_ready_queue` → take `g_rqlock` instead of BKL.
2. **Allocator lock** `g_memlock` — `memory.c:275,364` FreeQue → its own lock (it's leaf, no nesting → simple).
3. **Object-table locks** — per-object-class (sem/mtx/evtflag/mbox/msgbuf) or per-object-instance lock; the wait-queue inserts/removes. This is where lock-ordering discipline (always acquire in a fixed global order) becomes mandatory to avoid deadlock; `[smp-no-deadlock]` re-runs here.
4. **Per-CPU run-queues + migration/load-balancing** — the biggest perf lift and the biggest complexity; **DEFER hard** (§8). The global single ready-queue under `g_rqlock` is correct and simple first.

The discipline: **never split a lock until a cert shows the BKL version byte-identical AND a contention measurement shows the split is needed.** Coarse-but-correct beats fine-but-racy for a kernel that hosts an unownable mind.

---

## 3. Per-CPU scheduler state — `knl_ctxtsk`/`knl_schedtsk` become per-CPU

Today there is exactly ONE `knl_ctxtsk` (the running task) and ONE `knl_schedtsk` (the next task) — defined `task.c:62-63`. This single pair **is the uniprocessor assumption** (the dispatcher `cpu_support.S:90-124` loads them as globals; `END_CRITICAL_SECTION` dispatches on `knl_ctxtsk != knl_schedtsk`, `arch/aarch64/include/cpu_status.h:19`). ② makes them per-CPU.

### 3.1 The per-CPU block (extend MC-2's `g_cpu[]`)

Extend MC-2's `struct pk_cpu g_cpu[PK_SMP_MAX_CPUS]` (`mc2_smp.c:38-60`, already 4 slots, slots 2/3 reserved) with the scheduler fields:

```c
struct pk_cpu {                    /* EXISTING MC-2 fields (mc2_smp.c:38-44) */
    unsigned long stack_top;       /* off 0  (PK_CPU_STACK_TOP, asm-mirrored) */
    unsigned long cpu_id;          /* off 8  (mpidr Aff0) */
    volatile unsigned long woken;  /* off 16 */
    /* ── NEW for ② (append; keep existing offsets stable for start.S) ── */
    TCB *ctxtsk;                   /* this CPU's running task   (was global knl_ctxtsk) */
    TCB *schedtsk;                 /* this CPU's next task      (was global knl_schedtsk) */
    INT dispatch_disabled;         /* this CPU's dispatch gate  (was global) */
    UW  taskindp;                  /* this CPU's task-indep ctr (was global) */
    unsigned long idle_stack_top;  /* this CPU's idle/dispatch stack */
};
```

- **Indexing:** by **MPIDR Aff0** (the firmware-independent "which core am I", `mpidr_aff0()` per `mc2-1-ncore-equiv-plan.md` §3.2). The IRQ vector and dispatcher read `g_cpu[mpidr_aff0()]` instead of the globals.
- **`knl_ctxtsk`/`knl_schedtsk` become accessor macros** `CUR_CTXTSK` / `CUR_SCHEDTSK` resolving to `g_cpu[mpidr_aff0()].ctxtsk` etc. **Every reader/writer enumerated in §1.1 + the §Appendix** must switch from the global to the per-CPU accessor. This is the largest mechanical diff in ②; the "cert must cover ALL paths" lesson applies — miss one site and that CPU reads another CPU's running task. (The BKL means most readers are already serialized, so the per-CPU-ization can be staged: first BKL with the globals still single-writer-at-a-time, then per-CPU-ize when a second CPU actually runs the dispatcher.)
- **`knl_dispatch_disabled`, `knl_taskmode`, `knl_taskindp`** → per-CPU (the timer handler's `taskindp++/--` at `cpu_support.S:157-167` is inherently per-CPU; `knl_taskmode` is saved/restored per task already, just needs a per-CPU home for the live value).

### 3.2 Per-CPU idle

Each CPU's dispatcher needs its own `.Lidle` (`cpu_support.S:120-124`). Today there is one idle `wfe`. Under ②, when a CPU finds `g_cpu[me].schedtsk == NULL` it idles on `wfe` **and must be wakeable by an IPI** (§4) when another CPU makes a task runnable that this CPU should pick up. The dispatcher loop (`cpu_support.S:90-124`) loads `g_cpu[me].schedtsk` instead of the global `knl_schedtsk`, and the `.Lidle` `wfe` is re-checked after any IPI/`sev`. On the hosted port, `knl_idle_wait` (`preempt.c:142-156`) already sleeps until SIGALRM — the per-CPU analogue sleeps until SIGALRM **or** the reschedule signal (§4 hosted analogue).

### 3.3 The ready queue: ONE shared global queue under a lock (NOT per-CPU run-queues — yet)

**Pick the simplest-correct first:** keep the single global `knl_ready_queue` (`ready_queue.h:52`), protected by the BKL (②.0) then `g_rqlock` (②.3). Each CPU, when it needs work, locks the ready queue, pops the highest-priority runnable task, sets `g_cpu[me].schedtsk`, unlocks. Some contention, fully correct, and it **preserves the global strict-priority semantics** the uniprocessor has today (the highest-priority N runnable tasks run on the N CPUs).

**Per-CPU run-queues + migration/load-balancing are DEFERRED** (§8) — they are faster but vastly more complex (work-stealing, load metrics, affinity, the cross-CPU migration race) and they *change scheduling semantics* (a task may run on a lower-priority CPU's queue while a higher-priority task waits elsewhere). For a kernel hosting one mind, **global strict priority under one ready-queue lock is the right first SMP scheduler.**

### 3.4 The `klocktsk` / big-kernel-lock-of-the-OLD-design subtlety

`ready_queue.h:38` states the invariant *"Multiple READY tasks with kernel lock do not exist at the same time"* and `klock.c` implements `knl_LockOBJ`/`knl_UnlockOBJ` using `RDYQUE.klocktsk` (`ready_queue.h:49`) as the single ready task holding the kernel lock. **This is a second, pre-existing uniprocessor assumption distinct from §3.1.** Under SMP, `knl_ready_queue_top` returns `klocktsk` if set (`ready_queue.h:76-78`) — but with N CPUs, "the one task holding the kernel lock" must become "this is serialized by the BKL/`g_rqlock`". In the BKL wave this is automatically correct (only one CPU is in the kernel, so only one task transitions through `klocktsk` at a time). It only needs explicit attention at the finer-lock stage (②.3) — flag it there.

---

## 4. Cross-CPU dispatch = IPIs (the GIC has NO SGI path today — real new interrupt-controller work)

When CPU A wakes a task that is higher-priority than what CPU B is currently running (e.g. `knl_make_ready` makes a high-prio task runnable, `task.c:230`), A must tell B to reschedule. On a uniprocessor this never happened (one CPU). Under SMP, A sends B a **reschedule IPI**; B's SGI handler enters B's dispatcher and B picks up the higher-prio task.

### 4.1 The GIC has no SGI plumbing today — scope honestly

Grounded: the GIC init (`tkdev_init.c:93-109`) does distributor enable, the timer PPI (id 30), CPU-interface enable, priority mask — **and nothing else.** There is **no `GICD_SGIR` definition** anywhere (`tkdev_conf.h:33-39` defines `GICD_CTLR 0x000`, `GICD_ISENABLER 0x100`, `GICC_*` — but **no `GICD_SGIR` (offset 0xF00)**; grep confirms zero `SGIR`/`sgir` matches in `arch/aarch64/`). The IRQ vector (`cpu_support.S:234-284`) reads `GICC_IAR`, dispatches via `knl_intvec`, writes `GICC_EOIR` — it can **receive** an SGI today (INTIDs 0-15 are SGIs and would come through `GICC_IAR`), but nothing **sends** one and no handler is registered for SGI INTIDs.

**The honest ② lift (real interrupt-controller work):**
1. **Add `GICD_SGIR` (offset 0xF00)** to `tkdev_conf.h` and a `gic_send_sgi(cpu_target_mask, sgi_id)` helper: write `GICD_BASE + 0xF00` with `(target_list << 16) | sgi_id` (GICv2 SGIR format). Pick e.g. SGI INTID 0 = "reschedule".
2. **Per-CPU GIC CPU-interface init for secondaries.** Today `gic_init` (`tkdev_init.c:93-109`) inits the distributor (shared, once) **and the CPU interface (`GICC_CTLR`, `GICC_PMR`) — but the CPU interface is PER-CPU.** Each secondary must enable **its own** `GICC_CTLR`/`GICC_PMR` after bringup (the distributor is shared and inited once by the primary). This is new per-CPU bringup code on top of MC-2's EL1 setup (`start.S:220-260`).
3. **Register an SGI handler** in `knl_intvec` for the reschedule SGI INTID. The handler (running on the target CPU, in IRQ context) simply ensures the dispatcher re-evaluates `g_cpu[me].schedtsk` on return — i.e. it requests a dispatch on this CPU (set a per-CPU "resched pending", let `END_CRITICAL_SECTION`/the IRQ-return path dispatch). The `_vec_el1_irq` path (`cpu_support.S:234-284`) already EOIRs and returns — the SGI handler slots into the existing `knl_intvec` dispatch (`cpu_support.S:264-269`).
4. **`knl_reschedule` becomes IPI-aware.** Today `knl_reschedule` (`task.h:240-249`) sets the global `knl_schedtsk` and calls the no-op `knl_dispatch_request()`. Under ②, when it raises a task to highest-priority and the target CPU to run it is **not** the current CPU, it must `gic_send_sgi(target, RESCHED_SGI)`. Deciding *which* CPU should preempt (the lowest-priority currently-running CPU) requires scanning `g_cpu[].ctxtsk` priorities — a small cross-CPU read under `g_rqlock`.

### 4.2 Hosted-port IPI analogue

On hosted, the "CPUs" are threads; the IPI is a **signal to the target thread** (e.g. `pthread_kill(target_tid, SIGRESCHED)` with a real-time signal, or a per-thread eventfd/condvar). The target thread's handler does the same "request dispatch on return" as the SGI handler. This mirrors the existing `SIGEV_THREAD_ID` per-thread signal targeting (`preempt.c:216-225`).

### 4.3 ②.0/②.1 can DEFER IPIs partially

In ②.0 (BKL, 2 CPUs each running the dispatcher with the **timer tick on each CPU** driving its own preemption), a CPU re-evaluates `schedtsk` on **every local timer tick** anyway. So cross-CPU preemption *latency* is bounded by one tick even without IPIs. IPIs (§4) reduce that latency to immediate and are required for correctness only when a wake must preempt **right now** (a high-prio task should not wait up to a tick on another CPU). **②.1 adds the IPI/SGI path**; ②.0 can ship with tick-bounded cross-CPU preemption (honest: slightly higher worst-case preemption latency, but correct). State this in the ②.0 cert.

---

## 5. Bringup: secondaries run the DISPATCHER, not a tile loop

MC-2's `_secondary_worker` (`start.S:172-264`) brings a secondary up (EL2→EL1 drop, `_secondary_el1_setup`: SCTLR caches+FP, **SMPEN=1**, shared `el1_vectors`, own stack) and enters `pk_smp_worker_loop` — **a fixed tile loop that never touches the scheduler.** ② generalizes the *tail*: instead of `pk_smp_worker_loop`, a secondary enters the **T-Kernel dispatcher** (`.Ldispatch_loop`, `cpu_support.S:90`) with:
- its own per-CPU `g_cpu[me].ctxtsk`/`schedtsk` (§3),
- its own stack (already provided by MC-2's per-slot stacks, `mc2-1-ncore-equiv-plan.md` §3.5),
- its own **GIC CPU-interface init** (§4.1.2) and its own **timer tick** (each CPU programs its own EL1 physical timer `CNTP_*` — the timer is a per-CPU PPI (id 30), so each core gets its own tick that drives its own preemption; `timer_init` at `tkdev_init.c:117` becomes per-CPU).

**The boot CPU starts the others once the scheduler is SMP-ready:** primary boots, inits the kernel single-threaded (as today), inits the distributor + BKL + per-CPU blocks, **then** releases secondaries via `mc2_smp_release_n()` (`mc2-1-ncore-equiv-plan.md` §3.1) pointing them at a NEW `_secondary_dispatch_entry` (the generalized landing pad) instead of `_secondary_worker`. Each secondary does its EL1 setup (reuse `_secondary_el1_setup`, `start.S:220`), inits its GIC CPU-interface + timer, then `b .Ldispatch_loop` with `g_cpu[me]` as its scheduler state. From then on N cores each pull from the global ready queue under the BKL.

**MMU-off simplification carries over:** VA==PA on all cores (`mc2-baremetal-smp-plan.md` §1.2) — no page-table coherency handshake. SMPEN=1 on every core (already in MC-2's `_secondary_el1_setup`, `start.S:247-249`) makes the BKL spinlock + shared ready-queue cacheable accesses coherent. This is the same coherency obligation MC-2 already discharged; ② inherits it.

---

## 6. THE CERTS (cert-first, falsifiable) — each with a falsifier that MUST go RED

All ②.0–②.2 certs are **QEMU `-smp 4`-testable** (extend the existing `run-smp` target + the boot-self-test harness pattern, `mc2-1-ncore-equiv-plan.md` §4.3, `tests/aarch64/run_mc2_*.sh`). The **barrier/coherency teeth are `[live]`-only on RPi3** — the same MC-2 honesty (QEMU TCG models memory strongly and may MASK a missing-barrier/SMPEN race).

### (a) `[smp-N-tasks-run]` — N CPUs provably run DISTINCT tasks concurrently — **QEMU `-smp 4`**
**Claim:** with N=2 (②.0) then N=4 (②.1), each CPU runs a distinct schedulable T-Kernel task at the same time.
**Harness:** create N busy tasks, each landing on a distinct CPU; each increments a **per-CPU execution counter** (the MC-2.1 per-slot instrumentation idiom, `g_cpu[]` counters). After a bounded run, assert **every** `g_cpu[i].exec_count > 0` for i in 0..N-1, and the per-CPU running-task ids are **distinct** (read `g_cpu[i].ctxtsk->tskid`). Print `SMP-NRUN: PASS/FAIL` over UART; grep harness.
**Falsifier (MUST go RED):** don't release secondaries / don't per-CPU-ize `schedtsk` → only `g_cpu[0]` advances → `SMP-NRUN: FAIL`. Proves the cert isn't vacuous (it's actually observing N cores executing).

### (b) `[smp-mutual-exclusion]` — the locks work (no lost updates) — **QEMU `-smp 4`**
**Claim:** a shared counter incremented `K` times by tasks spread across all CPUs reaches **exactly `N*K`** (no lost updates → mutual exclusion holds).
**Harness:** N tasks, each does `K` increments of a single shared global **through a kernel-locked path** (e.g. via a semaphore-guarded section, or directly under the BKL). Assert final == `N*K` exactly. Print `SMP-MUTEX: PASS/FAIL`.
**Falsifier (MUST go RED):** a `-DSMP_MUTEX_NOLOCK` variant that increments the shared counter **without** taking the lock (plain `g++`). Under `-smp 4` with truly concurrent tasks this MUST lose updates → final `< N*K` → `SMP-MUTEX: FAIL`. **Honest caveat:** on a fast-enough non-contended interleaving QEMU *might* occasionally not lose an update — so the no-lock falsifier should run a **high iteration count** to make the lost update statistically certain, and the full teeth (real store-buffer races) are RPi3-`[live]`. The locked version must be `N*K` **every** run.

### (c) `[smp-one-mind]` — THE CROWN: SMP did not perturb the mind's math — **QEMU `-smp 4`** (+ RPi3 `[live]` for full coherency teeth)
**Claim:** the distributed mind's output (a forward, or a DMN consolidation, or a `gl_merge`) is **BYTE-IDENTICAL** whether the kernel runs uniprocessor (`-smp 1` / today's build) or SMP (`-smp 4`). Proves ② did not change a single bit of the mind.
**Harness:** run a fixed-seed mind operation in-kernel under both configs:
1. **Uniprocessor reference:** today's build, compute the mind output `y_uni`, FNV-1a hash it (the `student_shell.c:691-694` idiom MC-2 reuses).
2. **SMP build:** same fixed seed/inputs under `-smp 4`, compute `y_smp`, hash it.
3. Assert `memcmp(y_uni, y_smp) == 0` and `hash_uni == hash_smp`. Print `SMP-ONEMIND: PASS/FAIL`.
The candidate operations, in order of strength: (i) an `r_forward` (`r3_incontext.c:192-258`, pure math — should be trivially identical, the warm-up cert); (ii) a DMN consolidation round (`lm_consolidate.c` replay — proves the engram ring + replay order didn't reorder); (iii) a `gl_merge` (`gossip_learn.c:69-75` — the float-order asterisk §1.3.1; assert byte-identical *given the same input-array order*, which proves ② didn't reorder the merge inputs).
**Falsifier (MUST go RED):** a deliberately-broken SMP build that lets two CPUs race the engram ring append **without** the lock (so append order becomes scheduling-dependent) → the consolidation input set reorders → `y_smp != y_uni` → `SMP-ONEMIND: FAIL`. This proves the cert actually guards the determinism (not vacuously passing because the op is single-threaded anyway).
**Honest caveat (MC-2 §4.4, restated):** a QEMU `[smp-one-mind]` PASS proves the **partition/append/replay determinism + that ② didn't reorder**; it does **NOT** prove the cache-coherency/barrier discipline (QEMU may mask a missing `dsb ish`/SMPEN=0). That sub-claim is **`[live]`-only on RPi3** (MC-2.2 hardware). The plan must NOT claim "SMP byte-identity verified on hardware" from a QEMU green.

### (d) `[smp-no-deadlock]` / boot-survives — **QEMU `-smp 4`**
**Claim:** the SMP kernel boots all N CPUs into the dispatcher, runs a mixed workload (tasks taking sems/mutexes/mbox across CPUs), and **never deadlocks or livelocks** for a bounded wall-clock run; the BKL re-entrancy (timer IRQ nested in a syscall) does not self-deadlock.
**Harness:** boot to `[BOOT] Starting T-Kernel...` (`main.c`), release N CPUs, run a stress workload that exercises cross-CPU lock acquisition + the timer tick on each CPU, for a fixed wall-clock window; assert forward progress (a global heartbeat counter advances on all CPUs) and the run terminates cleanly. Print `SMP-NODEADLOCK: PASS/FAIL`.
**Falsifier (MUST go RED):** the §1.4 known corruption — a `-DSMP_NO_RQLOCK` variant that lets two CPUs race `knl_ready_queue_delete` → the documented `bitmap`/`top_priority` invariant break → `bitsearch1` livelock (`preempt.c:199-211`). This MUST hang/FAIL, proving the ready-queue lock is load-bearing. (Watchdog the harness so the livelock reports FAIL rather than hanging the CI.)

### Cert marking summary
| Cert | QEMU `-smp 4` | RPi3 `[live]` |
|---|---|---|
| `[smp-N-tasks-run]` | **YES** (full) | yes |
| `[smp-mutual-exclusion]` | **YES** (lock holds every run; no-lock falsifier teeth strongest on HW) | full teeth |
| `[smp-one-mind]` (crown) | **YES** for partition/append/replay determinism | **ONLY here** for the barrier/SMPEN coherency sub-claim |
| `[smp-no-deadlock]` | **YES** (incl. the §1.4 livelock falsifier) | yes |

---

## 7. Sequencing — small, falsifiable, each independently shippable

Each wave: a falsifiable cert + a falsifier that must go RED, on an **explicit-hash base**, via a **separate impl→audit cycle** (implementer ≠ auditor ≠ commander; MEMORY.md NON-NEGOTIABLE constitution).

**②.0 — 2 CPUs, Big Kernel Lock, BOTH run the dispatcher. (THE SMALLEST REAL SLICE.)**
- Add `g_bkl` (recursive ticket spinlock, MC-2 `ldaxr/stlxr`+`stlr`+`wfe/sev` discipline, `mc2_smp.c:110-129`); wrap `BEGIN/END_CRITICAL_SECTION` + `BEGIN/END_DISABLE_INTERRUPT` (`arch/aarch64/include/cpu_status.h:17,30`) to take/release it; hand-convert the bare `DI(imask)` in `memory.c:275,364`.
- Per-CPU `g_cpu[me].ctxtsk/schedtsk/dispatch_disabled/taskindp` (§3.1); accessor macros; per-CPU idle (§3.2). Global single ready-queue under the BKL (§3.3).
- Generalize MC-2's `_secondary_worker`→`_secondary_dispatch_entry` (§5): release ONE secondary into `.Ldispatch_loop` with its own stack + GIC CPU-iface init + own timer tick.
- **No IPIs yet** — cross-CPU preemption is tick-bounded (§4.3, honest latency note).
- **Certs:** `[smp-2-tasks-run]` + `[smp-mutual-exclusion]` + `[smp-no-deadlock]` on QEMU `-smp 4` (use 2 of the 4); scheduler still correct (the existing uniprocessor tests still green under `-smp 1`). *Proves two cores each run the real dispatcher under one lock and the kernel survives it — the first SMP scheduler step.*

**②.1 — IPI reschedule + preemption across CPUs + N=4.**
- Add `GICD_SGIR` + `gic_send_sgi` + per-CPU GIC CPU-iface init + the reschedule-SGI handler (§4.1); make `knl_reschedule` IPI-aware (§4.1.4). Hosted analogue = per-thread signal (§4.2).
- Release all N-1 secondaries (reuse `mc2_smp_release_n`).
- **Certs:** `[smp-N-tasks-run]` at N=4; cross-CPU preemption latency now immediate (a wake on A preempts B at once, verified by a latency counter); `[smp-no-deadlock]` re-run at N=4.

**②.2 — THE `[smp-one-mind]` byte-identity CROWN cert.**
- Run the real student `r_forward` → DMN consolidation → `gl_merge` under SMP; prove **byte-identical** to uniprocessor (§6c). Ship the append-race falsifier.
- This is the gate that says "② did not split the mind." Until it's green, ② is not trustworthy. Honest QEMU-masks-coherency caveat stated; barrier sub-claim `[live]`-deferred to RPi3.

**②.3 — finer locks + per-CPU run-queues + migration (the PERF wave). DEFER.**
- Split BKL → `g_rqlock` → `g_memlock` → object-table locks (§2.2), each its own cert + lock-ordering `[smp-no-deadlock]`. Then per-CPU run-queues + migration/load-balancing (the biggest complexity; §8). Re-run `[smp-one-mind]` after **every** split — coarse-correct must stay byte-identical as it's refined.

**hosted-port SMP (threads-as-CPUs) parity — separate lift (§8).**
- Recursive `pthread_mutex` BKL; per-thread reschedule signal; N kernel threads each running the dispatcher. The `SIGEV_THREAD_ID` generalization (§2.1, §4.2). Mark which certs port (all of §6 have a hosted form).

---

## 8. The honest cost + the relationship to MC-2.2 + what stays DEFERRED

**This is the biggest change in the repo, and it can break the "one mind."** Be brutally honest:

1. **It took Linux ~10 years.** p-kernel's disciplined version is tractable for ONE reason: **the mind's heavy-math determinism is already solved** (MC-2 output-partition + the canonical append/merge/replay orders, §1.3). That collapses ②'s determinism problem to a **kernel-state race** problem, which the **BKL closes wholesale in ②.0** by making the kernel internally single-threaded again. p-kernel ships **coarse-correct first, refines by measured contention** — the opposite of building a general-purpose fine-grained SMP scheduler up front. That is why the small-wave version is far faster than Linux's.

2. **The barrier/coherency teeth are `[live]`-only on RPi3.** QEMU TCG models memory strongly and may MASK a missing-`dsb ish`/SMPEN=0 race (MC-2 §4.4). So **MC-2.2 hardware validation is effectively a prerequisite for TRUSTING ② on real hardware**, even though ②.0–②.2 develop on QEMU. A green QEMU `[smp-one-mind]` proves determinism-of-reordering; it does NOT prove the locks' barriers are correct on weakly-ordered silicon. ②'s RPi3 cert (the BKL/`g_rqlock` barrier falsifier) inherits MC-2.2's hardware on-ramp (`make tftp`/`make run-rpi3`).

3. **What ② must NOT silently lose (the uniprocessor's current guarantees):**
   - **Global strict priority** — the highest-priority runnable task always runs. ②.0 with one global ready-queue preserves it (the top-N run on N CPUs); per-CPU run-queues (②.3) would *change* this — flag it as a semantic decision, not a silent regression.
   - **`tk_dis_dsp()` dispatch-disable semantics** (`knl_dispatch_disabled`) — must become per-CPU and still mean "this task won't be preempted *on its CPU*"; it can no longer mean "the whole system won't dispatch" (it never can under SMP) — a documented semantic narrowing.
   - **Critical-section atomicity** — every `BEGIN_CRITICAL_SECTION` region that was atomic on a uniprocessor must remain atomic under the BKL. The "cert must cover ALL paths" lesson: enumerate every critical section and the bare `DI` site (`memory.c:275,364`); miss one and that race is silent.
   - **Byte-identity of the mind** — the crown. `[smp-one-mind]` (§6c) is the standing guard; re-run after every lock-split.

4. **DEFERRED / OUT-OF-SCOPE (explicit):**
   - **Per-CPU run-queue load-balancing / work-stealing / affinity / NUMA** — ②.3 perf wave; the global single ready-queue is correct and simple first.
   - **CPU hotplug** (offlining/onlining cores at runtime) — not needed; N is fixed at boot (4).
   - **Hosted-port SMP** — a separate lift (recursive pthread BKL + per-thread reschedule signal); the hosted port currently *relies on* single-threaded kernel entry (the `SIGEV_THREAD_ID` fix, `preempt.c:216-225`). Bringing the hosted port to true N-thread SMP is its own wave.
   - **x86_64 bare-metal SMP** (INIT-SIPI-SIPI AP startup) — MC-2 is aarch64-only; ② likewise starts aarch64. x86 hosted is the threads model above.
   - **Splitting the matmul contraction across CPUs** — would reassociate the reduction, breaking byte-identity. Only output-row partitioning (MC-2's, already shipped) is ever used.

5. **Relationship to MC-2 / MC-2.1 (what ② inherits vs adds):**
   - **Inherits (no re-derivation):** secondary bringup (PSCI CPU_ON, `mc2_smp.c:198-237`), per-CPU `g_cpu[]` (`mc2_smp.c:38-60`), the `ldaxr/stlxr`+`stlr` lock (`mc2_smp.c:110-129`), SMPEN (`mc2_smp.c:137-144`, `start.S:247-249`), the `dsb ish`/`dmb` barrier discipline, per-CPU stacks (`mc2-1-ncore-equiv-plan.md` §3.5), the `-smp 4` run target + boot-self-test + grep harness pattern (`tests/aarch64/run_mc2_*.sh`).
   - **Adds (the ② lift MC-2 deliberately stopped short of):** the BKL/finer locks replacing DI/EI (§2); per-CPU `knl_ctxtsk`/`knl_schedtsk` + per-CPU idle (§3); secondaries running the **dispatcher** not a tile loop (§5); the **GIC SGI/IPI** path that does not exist today (§4); per-CPU GIC CPU-interface + per-CPU timer tick (§4.1.2, §5).

---

## Appendix — grounding (file:line, all on trunk `432cf337`)

**The uniprocessor scheduler ② must make SMP:**
- Scheduler globals: `kernel/common/task.c:62` (`knl_ctxtsk`), `:63` (`knl_schedtsk`), `:55` (`knl_dispatch_disabled`), `:64` (`knl_ready_queue`). The "one running / one next" pair is the uniprocessor assumption.
- Reschedule / dispatch-request: `include/kernel/tkernel/task.h:240-249` (`knl_reschedule` sets `knl_schedtsk`, calls no-op `knl_dispatch_request`); `knl_dispatch_request()` = `/* */` no-op at `include/kernel/tkernel/cpu_status.h:104`; actual dispatch deferred to `END_CRITICAL_SECTION` (`arch/aarch64/include/cpu_status.h:19-22`).
- `knl_make_ready`→`knl_reschedule` set `schedtsk` when a task becomes top: `task.c:230`; `knl_make_non_ready`: `task.c:257`.
- Ready queue (bitmap + per-prio + `top_priority` + `klocktsk`): `include/kernel/tkernel/ready_queue.h:44-50`; insert/delete (the `bitmap`/`top_priority` invariant) `:98-173`; **uniprocessor "one kernel-locked ready task" invariant comment `:38`**; `knl_ready_queue_top` returns `klocktsk` if set `:73-81`.
- The dispatcher + per-CPU `.Lidle`: `arch/aarch64/cpu_support.S:90-124` (loads global `knl_schedtsk`/`knl_ctxtsk` `:91-96`, idle `wfe` `:120-124`); discard-and-switch `:48-53`; save path `:61-85`; clears `knl_dispatch_disabled` `:101-103`; timer-handler taskindp++/-- `:157-167`.
- The crown invariant + byte-identity build flags: `boot/aarch64/Makefile` `-O1 -ffp-contract=off`.

**The critical-section / lock primitives (DI/EI = the uniprocessor lock):**
- `BEGIN/END_CRITICAL_SECTION` = `disint()`/`enaint()` + dispatch-on-exit: `arch/aarch64/include/cpu_status.h:17-24`; `BEGIN/END_DISABLE_INTERRUPT` `:30-31`; generic `include/kernel/tkernel/cpu_status.h:29-41`.
- `disint`/`enaint` aarch64 (`mrs daif; msr daifset/clr #0x3`): `arch/aarch64/include/cpu_insn.h:18-35`.
- Allocator bare `DI(imask)` ("Exclusive control by interrupt disable"): `kernel/common/memory.c:275`, `:364`.
- `fastlock` Inc/Dec under DI/EI: `kernel/common/fastlock.c:61-64,84-87`; `fastmlock` INC/DEC/BTS/BR: `fastmlock.c:65-68,80-83,100-103,119-121`.
- `klock` (kernel object lock) `BEGIN_CRITICAL_SECTION`: `kernel/common/klock.c:55,98`.
- Subsystems using DI/EI critical sections (each a cross-CPU race under SMP): eventflag.c, mailbox.c, mempool.c, mempfix.c, messagebuf.c, mutex.c (all via `BEGIN_CRITICAL_SECTION`/`BEGIN_DISABLE_INTERRUPT`).
- Timer handler critical section over `knl_timer_queue` + `knl_current_time` + RR slice: `kernel/common/timer.c:177-231` (`BEGIN_CRITICAL_SECTION` `:183`, queue walk `:199-216`, RR `:219-226`, `END` `:228`).

**The hosted port (threads-as-CPUs; ②'s race ALREADY observed there):**
- `arch_irq_disabled_flag` = the hosted DI/EI flag, explicitly "cannot protect across threads": `arch/linux/aarch64/preempt.c:39`, banner `:24-26`.
- **The exact ② corruption observed + the retreat-fix:** `preempt.c:199-211` (concurrent `knl_timer_handler_startup` corrupts ready/timer queues → segfault + `bitsearch1` livelock under `knl_ready_queue_delete`); fixed by pinning to one thread `SIGEV_THREAD_ID` `:216-225`.
- `knl_idle_wait` (the hosted per-CPU idle analogue): `preempt.c:142-156`; SIGALRM handler `:158-176`.
- disint/enaint hosted (flag-based, no privileged insn): `arch/linux/{aarch64,x86_64}/include/cpu_insn.h:20-35`.

**The GIC SGI/IPI gap (real new ② work):**
- GIC init — distributor + timer PPI + CPU-iface + PMR, **nothing else**: `arch/aarch64/tkdev_init.c:93-109`; `gic_enable_irq` `:80-86`.
- **No `GICD_SGIR`:** `tkdev_conf.h:33-39` defines `GICD_CTLR 0x000`, `GICD_ISENABLER 0x100`, `GICC_CTLR/PMR/IAR/EOIR` — but no SGIR (0xF00); grep confirms zero `SGIR` in `arch/aarch64/`. Bases: `GICD_BASE 0x08000000`, `GICC_BASE 0x08010000` (QEMU virt), `tkdev_conf.h:28-29`.
- IRQ vector (can receive SGI via IAR, nothing sends): `arch/aarch64/cpu_support.S:234-284` (IAR read `:256`, `knl_intvec` dispatch `:264-269`, EOIR `:280`).
- Per-CPU timer (PPI 30) `timer_init`: `tkdev_init.c:117`.

**The MC-2 substrate ② builds on (already shipped + audited):**
- Plans: `docs/architecture/mc2-baremetal-smp-plan.md` (§3.4 carry-over table, the safety boundary), `docs/architecture/mc2-1-ncore-equiv-plan.md` (§3.1 N-core release, §3.2 mpidr slot, §3.5 per-CPU stacks, §7 ② carry-over).
- per-CPU `g_cpu[]` (4 slots, 2/3 reserved): `arch/aarch64/mc2_smp.c:38-60`; the `ldaxr/stlxr`+`stlr`+`wfe/sev` lock `:110-129`; SMPEN `:137-144`; PSCI CPU_ON release `:198-237`; landing pad + EL1 setup `arch/aarch64/start.S:172-264` (SMPEN asm `:247-249`); shared `el1_vectors` `cpu_support.S:210-212`.

**The determinism-critical mind consumers (already insulated — §1.3):**
- DMN ring append by deterministic stride + deterministic tiebreaker: `arch/common/lm_consolidate.c:296-306`, `:322-327`; canonical replay loop `:402-407`; DMN tick cadence (wall-clock + pulse ctr) `arch/common/dmn.c:468-494`.
- `gl_merge` order-independent SUM + the ~1e-6 IEEE asterisk: `arch/common/gossip_learn.c:69-75`, order-indep comment `:114-115`, `[g22-no-central]` test `:481-512`.
- `r_forward` pure math: `arch/common/r3_incontext.c:192-258`; fact queue FIFO-by-seq `s_round`: `:1419-1420`, minibatch cycle `:1437-1447`.
- The crown ("one mind, one math" byte-identity, `-ffp-contract=off`): per MEMORY.md wave-49; canonical ascending reduction `arch/common/llm/student.c` (ASCENDING-order).

---

**Note:** This plan is a DESIGN PLAN by an automated design-harden on trunk `432cf337`. ②'s implementation is a 本丸-level decision **awaiting mk_pino's go-ahead** + per-wave **separate impl→audit cycles** (implementer ≠ auditor ≠ commander; the development METHOD is the project's immune system). This plan makes ② READY and de-risked — it does **not** start it. The smallest real first slice is **②.0** (2 CPUs, Big Kernel Lock, both run the dispatcher, `[smp-2-tasks-run]` + `[smp-mutual-exclusion]` + `[smp-no-deadlock]` on QEMU `-smp 4`).
