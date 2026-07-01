# MC-2.1 — N-core deterministic work-queue + the [mc2-smp-equiv] byte-identity cert: implementation plan (cert-first)

**Status: DESIGN PLAN** by an automated design-harden on commit `488d4232` (`488d4232 MC-2.0: secondary-core bringup + -smp 4 boot-survives cert (impl, pre-audit salvage)`, branch `mc2_0_impl`, on base `280c9887`). Read-only on code; no implementation in this wave. **Awaiting MC-2.0 merge + commander review + a separate impl→audit cycle.**

MC-2.0 proved exactly one thing: *a single parked aarch64 secondary can be woken via PSCI CPU_ON, run one trivial fixed tile under one lock, and the primary survives it* (`[mc2-boot-survives]`, `tests/aarch64/run_mc2_boot.sh`). It did **not** prove the *deterministic-worker mechanism* — that N cores splitting a real matmul produce the same bits as the serial loop. That is what MC-2.1 ships, **cert-first**: the falsifiable byte-identity gate `[mc2-smp-equiv]`.

The whole plan leads with that cert and the one barrier surface it certifies. The honest boundary — **QEMU TCG models memory strongly and may MASK a missing-barrier/SMPEN race**, so the falsifier's teeth on the *barrier discipline* are only fully load-bearing `[live]` on real RPi3 (MC-2.2) — is stated in §4 and never softened.

---

## 0. Read this first: what MC-2.1 is, and is NOT (carried from the parent plan §0)

The parent plan's "honest elephant" is unchanged and must be restated, because it reframes the cert:

- **Bare-metal aarch64 has no big matmul today.** The LLM "身体" tier (`pk_parallel.c`, the teacher's 49152-row head, 1536-row ffn) is **HOSTED-ONLY**: `pk_parallel.c` is built only in `boot/linux/Makefile:249`, never in `boot/aarch64/Makefile` (its `COMMON_C_SRCS` at `boot/aarch64/Makefile:115-167` does NOT list `pk_parallel.c`). Confirmed by grep: **zero `pk_parallel` references** in the bare-metal-built `arch/common/dtr.c`, `moe.c`, `r3_incontext.c`.
- The only bare-metal matmul is `dt_linear` (`arch/common/dtr.c:148-157`), and at R3's `R_DM≈48` it is ~2304 MACs — three orders of magnitude below the shipped 524288-MAC gate (`pk_parallel.h:65`). Parallelizing it would be *slower*.

**Therefore the MC-2.1 cert vehicle is a SYNTHETIC, gate-exceeding matmul in a boot self-test** (parent plan §3.1 "HONEST", §4.1) — *not* a real workload. MC-2.1 proves the *mechanism* (the partition arithmetic + the worker fork-join + the FP determinism) on a fixed-seed synthetic `W·x`, so that when SS-7 grows R3 past the gate the mechanism is already certified. **Do not let "N-core matmul" read to mk_pino as "the baby is faster today." It is not.** What MC-2.1 delivers is: *the parked cores wake, each computes a deterministic SLICE of one matmul, and the reassembled output is byte-identical to the serial loop* — the deterministic-worker mechanism ② reuses.

---

## 1. Ground truth: exactly what MC-2.0 (commit 488d4232) already gives MC-2.1

This is the load-bearing inventory. MC-2.1 must **reuse**, not re-derive, every primitive below. File:line is the MC-2.0 impl as it stands on `488d4232`.

### 1.1 The per-CPU block already reserves all 4 slots — REUSE

`arch/aarch64/mc2_smp.c:38-60`:
```c
#define PK_SMP_MAX_CPUS   4           /* QEMU -smp 4 / RPi3 = 4 cores */
struct pk_cpu {
    unsigned long stack_top;          /* off 0 */
    unsigned long cpu_id;             /* off 8: mpidr Aff0 (1..N-1) */
    volatile unsigned long woken;     /* off 16 */
};
struct pk_cpu g_cpu[PK_SMP_MAX_CPUS];   /* slot 0 primary; 1 = MC-2.0; 2/3 RESERVED */
```
The offsets are mirrored into `start.S` via `PK_CPU_STACK_TOP=0` (`mc2_smp.c:46-50`), guarded by four `_Static_assert`s (`mc2_smp.c:52-55`). **MC-2.1 REUSES `struct pk_cpu`, `g_cpu[]`, the offset macros, and the static asserts verbatim** — slots 2 and 3 are already there (`mc2_smp.c:57-59` comment: "Slots 2/3 reserved for MC-2.1 (N cores)").

### 1.2 The ldaxr/stlxr lock + the SMPEN set + the barrier idioms — REUSE

- **The one spinlock** `mc2_lock`/`mc2_unlock` (`mc2_smp.c:110-129`): `ldaxr`/`stxr` exclusive pair with `sevl`/`wfe` backoff; release is `stlr wzr` + `sev`. **MC-2.1 reuses this exact lock** for the work-queue critical sections (publish job, bump `done`).
- **`mc2_set_smpen()`** (`mc2_smp.c:137-144`): sets `CPUECTLR_EL1.SMPEN` (`S3_1_C15_C2_1`, bit 6) + `isb`. Called from the worker (`mc2_smp.c:155`) and the primary (`mc2_smp.c:221`). **REUSE unchanged.**
- **The barrier idioms already in use**: `dsb ish` to publish the per-CPU block (`mc2_smp.c:229`), `dmb st` to release worker stores (`mc2_smp.c:159,175`), `dmb ld` to acquire before reads (`mc2_smp.c:258,268,277,285`). **MC-2.1 reuses the SAME barrier vocabulary** — it adds a `dsb ish` *before the gen bump* and a `dmb ld` *after the join* (already the pattern in §3.3 of the parent plan; MC-2.0 has the pieces, MC-2.1 wires them into the gen/done protocol).

### 1.3 The PSCI CPU_ON release path — REUSE, generalize 1→N

`psci_cpu_on(target_mpidr, entry_pa, context_id)` (`mc2_smp.c:198-213`): issues `hvc #0` with `x0=0xC4000003` (SMC64 PSCI_CPU_ON, `mc2_smp.c:192`). `mc2_smp_release_one()` (`mc2_smp.c:218-237`) sets primary SMPEN, inits `g_cpu[1]`, `dsb ish`, then `psci_cpu_on(target=1, &_secondary_worker, &g_cpu[1])`. **MC-2.1 reuses `psci_cpu_on()` verbatim and LOOPS the body of `mc2_smp_release_one()` over slots 1..N-1** (§3.1).

### 1.4 The assembly landing pad + EL1 setup — REUSE unchanged

`start.S:172-264`:
- `_secondary_worker` (`start.S:173`): preserves `x0=context_id` (`&g_cpu[slot]`) in `x19`, drops EL2/EL3→EL1 (`_sec_from_el2`/`_sec_from_el3`, mirrors the primary's `_from_el2`/`_from_el3`), branches to `_secondary_el1_setup`.
- `_secondary_el1_setup` (`start.S:220-260`): installs the SHARED `el1_vectors` (defined in `cpu_support.S:211-212` — same vectors the primary uses), enables D/I-cache (NOT MMU bit 0 — VA==PA), enables FP (CPACR), **sets SMPEN in asm** (`start.S:247-249`), loads `sp` from `[x19, #0]` (PK_CPU_STACK_TOP), `bl pk_smp_worker_loop`.

**This entire landing pad is slot-agnostic already** — it takes `&g_cpu[slot]` in `x19`, loads that slot's stack, and the only slot-specific datum it touches is `stack_top`. **MC-2.1 REUSES `start.S` UNCHANGED.** The one thing the worker must NOT do is hardcode its slot — see §1.6.

### 1.5 The harness pattern + the `-smp 4` plumbing — REUSE, extend

- `boot/aarch64/Makefile`: `QEMU_SMP_FLAGS` with `-smp 4` (`:367-371`), targets `run-smp` (`:375-378`), `run-smp-fault` (`:383-386`), `mc2-test` (`:390-391`). The `-smp 4` is already correct for N=4. **MC-2.1 adds an `[mc2-smp-equiv]` build/run target** (or folds the equiv self-test into the same `MC2_SMP_SELFTEST` hook; §4.3).
- `main.c:27-35,109-150`: the `MC2_SMP_SELFTEST` hook — releases the secondary, joins, checks, prints `MC2-BOOT: PASS/FAIL`, then **falls through to the normal T-Kernel boot** (`main.c:147-153`). **MC-2.1 extends this hook** to additionally run the equiv self-test and print `MC2-EQUIV: PASS/FAIL` (§4.2).
- `tests/aarch64/run_mc2_boot.sh`: the grep-the-UART harness (build with `-DMC2_SMP_SELFTEST`, boot under `-smp 4` with a `timeout`, grep for `MC2-BOOT: PASS`). **MC-2.1 adds `tests/aarch64/run_mc2_equiv.sh`** modeled on this exact script, grepping `MC2-EQUIV: PASS` (§4.3).
- The FNV-1a idiom to reuse for the output hash: `arch/common/llm/student_shell.c:691-694` (`h = 1469598103934665603ULL; … h ^= p[i]; h *= 1099511628253ULL`), identical to `tests/llm/mc0_test.c:115-122`.

### 1.6 The ONE thing MC-2.0 hardcodes that MC-2.1 MUST generalize — `g_cpu[1]`

The MC-2.0 worker loop hardcodes slot 1:
```c
/* mc2_smp.c:151-158 */
void pk_smp_worker_loop(void) {
    mc2_set_smpen();
    g_cpu[1].woken = 1;            /* <-- HARDCODED slot 1 */
    ...
}
```
The parent plan §3.2 sketch derives the slot from MPIDR (`int slot = (int)(current_mpidr_aff0());`), but **`current_mpidr_aff0()` does NOT exist in the MC-2.0 impl** (grep: zero `mpidr`/`current_mpidr` reads in `mc2_smp.c` outside comments). **MC-2.1 MUST add a small MPIDR-Aff0 read helper** and change the worker to use its own slot, OR pass the slot via the per-CPU block (`g_cpu[slot].cpu_id` is already populated at release — `mc2_smp.c:226` sets `cpu_id=1`; MC-2.1 sets each slot's `cpu_id`). **The robust choice (recommended): read MPIDR Aff0 in the worker** (the firmware-independent ground truth of "which core am I"), assert it equals the slot the primary expected, and use it to index `g_cpu[]` and `pk_slice`. This is §3.2.

---

## 2. The deterministic invariant MC-2.1 must preserve — the crown, restated for N cores

The single non-negotiable invariant, identical to the shipped hosted seam (`pk_parallel.h:9-16`):

> **The parallel matmul produces a result BYTE-IDENTICAL to the serial loop, for ANY worker count (1, 2, 4) and ANY completion order — because only the outer output-row loop is split (never the contraction), each `y[i]` is written by exactly one worker (no shared accumulator), and the inner `acc += …` left-fold order is the serial order, verbatim.** (`-O1 -ffp-contract=off` mandated.)

For N cores specifically, the invariant decomposes into four obligations MC-2.1 must each hold, and each maps to a §4 cert clause:

| # | Obligation | Enforced by | Cert clause |
|---|---|---|---|
| O1 | **Output-row partition only** — never split the contraction dim `n` | the worker runs the UNMODIFIED `dt_linear`-shape inner loop over its row range | `[mc2-smp-equiv]` byte-identity (§4.2) |
| O2 | **Disjoint, total partition** — every `y[i]` written by exactly one worker, no gaps, no overlaps | the IDENTICAL `pk_slice(out, nw, slot, &i0, &i1)` (`pk_parallel.c:57-63`) with the ragged remainder on the LAST slice | the falsifier's "wrong range" path (§4.4) |
| O3 | **Same float ops in the same order** as serial | each `y[i]` is a single left-fold; row order does not change the bits | byte-identity across nw∈{1,2,4} (§4.2) |
| O4 | **The primary reads each completed `y[i]` correctly** across physical cores | SMPEN + the publish `dsb ish` / worker `dmb st` / primary join `dmb ld` (§3.3) | the **barrier/SMPEN falsifier** — full teeth only `[live]` on RPi3 (§4.4) |

O1–O3 are **pure arithmetic + scheduling** and are fully provable under QEMU TCG. **O4 is the cache-coherency obligation that did not exist on hosted** (pthreads share one coherent address space) and is the *only* clause QEMU may not fully exercise (§4.4). This split — what QEMU proves vs what only hardware proves — is the most important honesty in the plan.

**The partition function is reused VERBATIM.** `pk_slice` (`pk_parallel.c:57-63`) gives slice `s` the rows `[s·q, …)` with `q = out/nw` and the remainder `(out % nw)` on the LAST slice. The bare-metal worker MUST call a byte-for-byte copy of this function so the bits match both the hosted golden and the serial path. (It is a 4-line pure function; MC-2.1 copies it into the bare-metal TU rather than linking `pk_parallel.c`, which is hosted-only — see §3.4 on the duplication and how the cert guards drift.)

---

## 3. The design

### 3.1 Generalize 1→N secondaries (release loop)

MC-2.1 turns `mc2_smp_release_one()` (`mc2_smp.c:218-237`) into `mc2_smp_release_n(int n)`:

```c
/* NEW in mc2_smp.c — generalizes the MC-2.0 single release. */
long mc2_smp_release_n(int n)            /* n = total cores incl. primary; 2..4 */
{
    mc2_set_smpen();                     /* primary's SMPEN (REUSE mc2_smp.c:137) */
    for (int slot = 1; slot < n; slot++) {
        g_cpu[slot].stack_top = pk_stack_top_for(slot);   /* per-slot stack, §3.5 */
        g_cpu[slot].cpu_id    = (unsigned long)slot;
        g_cpu[slot].woken     = 0;
    }
    __asm__ volatile("dsb ish" ::: "memory");             /* publish the block */
    long rc = 0;
    for (int slot = 1; slot < n; slot++) {
        long r = psci_cpu_on((unsigned long)slot,         /* REUSE mc2_smp.c:198 */
                             (unsigned long)&_secondary_worker,
                             (unsigned long)&g_cpu[slot]);
        if (r != PSCI_SUCCESS && r != PSCI_ALREADY_ON) rc = r;  /* first error wins */
    }
    return rc;
}
```

- `target_mpidr = slot` is correct for QEMU virt cortex-a53 (Aff0 = core index, Aff1..3 = 0 — the same assumption MC-2.0 made at `mc2_smp.c:231-232`, now applied to slots 2,3).
- The landing pad (`_secondary_worker`) and EL1 setup are **UNCHANGED** (§1.4) — they already take `&g_cpu[slot]` in `x19`.
- **Honest:** PSCI CPU_ON for cores 2,3 on QEMU virt with `-smp 4` is assumed to work identically to core 1; the cert (§4) is what verifies all N actually wake (each sets `g_cpu[slot].woken`). If a core fails to wake, the equiv self-test sees `nw` workers' worth of output but a missing `done` increment → the bounded join (§3.3) times out → `MC2-EQUIV: FAIL` (not a hang).

### 3.2 The worker derives its OWN slot (close the §1.6 hardcode)

```c
/* NEW small helper — the firmware-independent "which core am I". */
static inline unsigned long mpidr_aff0(void)
{
    unsigned long v;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
    return v & 0xFFUL;                 /* Aff0; matches start.S:41-42 mask */
}
```

The MC-2.1 worker loop (replaces `pk_smp_worker_loop`, `mc2_smp.c:151-185`):

```c
void pk_smp_worker_loop(void)
{
    mc2_set_smpen();                              /* REUSE mc2_smp.c:137 */
    int slot = (int)mpidr_aff0();                 /* 1..N-1 — NOT hardcoded */
    g_cpu[slot].woken = 1;
    __asm__ volatile("dmb st" ::: "memory");      /* publish woken (REUSE :159) */
    __asm__ volatile("sev" ::: "memory");

    unsigned long seen = 0;
    for (;;) {
        while (g_wq.gen == seen)                  /* no new job */
            __asm__ volatile("wfe");              /* BLOCK — no busy-spin (§5) */
        seen = g_wq.gen;
        __asm__ volatile("dmb ld" ::: "memory");  /* ACQUIRE the published job */

        if (slot < g_wq.nw) {
            size_t i0, i1;
            pk_slice_bm(g_wq.out, g_wq.nw, slot, &i0, &i1);   /* SAME partition */
            if (i1 > i0)
                ((pk_row_body)g_wq.body)((void *)g_wq.ctx, i0, i1);  /* serial body */
        }
        __asm__ volatile("dmb st" ::: "memory");  /* RELEASE this worker's stores */

        mc2_lock();                                /* REUSE mc2_smp.c:110 */
        g_wq.done++;
        mc2_unlock();                              /* stlr+sev — wakes the join */
        __asm__ volatile("sev" ::: "memory");
    }
}
```

Note the worker **never returns** and **never touches `knl_ctxtsk`/`knl_schedtsk`** — the §7 safety boundary holds.

### 3.3 The ONE work-queue + the barrier discipline (where SMP correctness lives)

The MC-2.0 single `g_done` flag (`mc2_smp.c:96`) becomes the §3.3 N-slice work-queue (the parent plan §3.2 `struct pk_wq`, adapted to MC-2.0's primitives):

```c
/* NEW: the one global work-queue (BSS, VA==PA). */
struct pk_wq {
    volatile pk_row_body  body;     /* current job (the serial inner loop)     */
    volatile void        *ctx;      /* matmul operands                         */
    volatile size_t       out;      /* total output rows                       */
    volatile int          nw;       /* slices this dispatch partitioned in     */
    volatile unsigned long gen;     /* bumped once per dispatch (PUBLISH)      */
    volatile int          done;     /* slices finished (under the lock)        */
} g_wq;
/* the lock is the EXISTING g_lock + mc2_lock/mc2_unlock (mc2_smp.c:97,110-129) */
```

**Dispatch (primary = worker 0):**
```
1. mc2_lock()                              REUSE mc2_smp.c:110
2. g_wq.body/ctx/out/nw = job; g_wq.done = 0
3. dsb ish                                 ; job fields land inner-shareable BEFORE gen bump  (REUSE the :229 idiom, new placement)
4. g_wq.gen++                              ; PUBLISH
5. mc2_unlock()                            ; stlr release on the lock word     REUSE mc2_smp.c:125
6. sev                                     ; wake all workers from wfe
7. pk_slice_bm(out, nw, 0, &i0, &i1); body(ctx,i0,i1)   ; primary runs slice 0 ITSELF
8. dmb st                                  ; primary's own slice-0 stores visible
9. wq_join(nw)                             ; bounded: while (g_wq.done < nw-1) wfe ; ceiling watchdog (REUSE mc2_smp.c:249-266)
10. dmb ld                                 ; ACQUIRE before reading any y[i]    REUSE mc2_smp.c:268
```

**Worker (secondary):** §3.2 — `wfe` until `gen` advances, `dmb ld` (acquire) to observe the job, run its slice, `dmb st` (release), then `g_wq.done++` under the lock + `sev`.

**Memory-ordering rationale (the load-bearing table — parent plan §3.3, verified against the MC-2.0 primitives):**

| Hazard | The barrier that prevents it | Where MC-2.0 already has the idiom |
|---|---|---|
| Worker sees new `gen` but stale `body`/`ctx`/`out`/`nw` | primary `dsb ish` (step 3) BEFORE `gen++`; worker `dmb ld` (acquire) AFTER reading `gen` | `dsb ish` at `mc2_smp.c:229`; `dmb ld` at `:258` |
| Primary reads a worker's `y[i]` before its store is globally visible | worker `dmb st` (release) after body; primary `dmb ld` (step 10) before reading output | `dmb st` at `mc2_smp.c:175`; `dmb ld` at `:268` |
| Two cores race the `done` counter | `done++` inside `mc2_lock`/`mc2_unlock`; release is `stlr`, acquire is `ldaxr` | lock at `mc2_smp.c:110-129` |
| Caches not coherent across cores at all | **CPUECTLR_EL1.SMPEN=1 on every core** — without it `dsb ish`/`dmb` operate on a non-shared domain and the scheme is **unsound on real hardware** | `mc2_set_smpen()` at `mc2_smp.c:137`, asm at `start.S:247-249` |

Use **inner-shareable (`ish`)** domain for `dsb` (all 4 cores are one cluster). `sev`/`wfe` is the *wake* mechanism; **the barriers are the correctness** — `sev` carries no data-ordering guarantee, so a `sev` without the preceding `dsb ish` is exactly the falsifier's bug (§4.4).

### 3.4 The bare-metal `pk_parallel_rows` backend behind the SHIPPED seam

MC-2.1 provides a **second implementation** of the shipped contract (`pk_parallel.h:104`):
```c
void pk_parallel_rows(size_t out, pk_row_body body, void *ctx);
```
- Hosted (`pk_parallel.c:164-206`): pthread pool. **Unchanged, hosted-only.**
- Bare-metal aarch64 (NEW, in `arch/aarch64/mc2_smp.c` or a sibling `pk_parallel_smp.c`): the secondary-core dispatch of §3.3. It runs slice 0 on the primary and slices 1..nw-1 on the woken secondaries.

The bare-metal backend honors the same fallback contract: if `nw<=1` or `out < PK_PARALLEL_MIN_ROWS` (64, `pk_parallel.h:45`) it calls `body(ctx,0,out)` inline — **byte-identical, zero overhead, no secondary touched** (mirrors `pk_parallel.c:171-180`). So the seam is safe even when the secondaries are asleep.

**The partition function duplication (honest).** `pk_parallel.c` is hosted-only (`pk_parallel.h:18-24`, `boot/aarch64/Makefile` excludes it). So the bare-metal TU **cannot link `pk_slice`** — it must contain a **byte-for-byte copy**, `pk_slice_bm`, of `pk_parallel.c:57-63`. This is a real drift surface (parent plan's "one mind, one math" risk class). **The cert guards it directly:** the equiv self-test computes `y_serial` via the *same* `pk_slice_bm(out, 1, 0, …)` path AND the hosted MC-0 cert (`tests/llm/run_mc0.sh`) pins the hosted `pk_slice`; if the two partitions ever diverge, the bare-metal byte-identity vs a hand-checked serial loop breaks. A stronger guard (recommended in §6): a `_Static_assert`-style unit check in the self-test that `pk_slice_bm` and an inline reference partition agree for `(out,nw)∈{(2048,2),(2048,4)}` and a ragged `(2047,4)`.

**Wiring `dt_linear` to the seam is DEFERRED** (parent plan §3.1 HONEST, §6). The cert uses a synthetic self-test matmul; wiring the real `dt_linear` (`dtr.c:148-157`) pays off only when R3 grows past the gate (SS-7). MC-2.1 does NOT modify `dtr.c`.

### 3.5 Per-secondary stacks in `linker.ld` (the concrete new memory)

MC-2.0 added ONE secondary stack region (`linker.ld:49-56`, `_stack_top_cpu1`, 16KB). **MC-2.1 adds `_stack_top_cpu2` and `_stack_top_cpu3`** (16KB each — workers run a leaf matmul body, no deep call chain), immediately after the cpu1 block:

```ld
/* linker.ld — after _stack_top_cpu1 (the MC-2.0 region at :49-56) */
    . = ALIGN(16);
    _stack_start_cpu2 = .;
    . += 0x4000;
    _stack_top_cpu2 = .;
    . = ALIGN(16);
    _stack_start_cpu3 = .;
    . += 0x4000;
    _stack_top_cpu3 = .;
```

`pk_stack_top_for(slot)` (§3.1) maps slot→stack symbol:
```c
extern unsigned char _stack_top_cpu1[], _stack_top_cpu2[], _stack_top_cpu3[];
static unsigned long pk_stack_top_for(int slot) {
    switch (slot) {
        case 1: return (unsigned long)_stack_top_cpu1;   /* REUSE MC-2.0 region */
        case 2: return (unsigned long)_stack_top_cpu2;   /* NEW */
        case 3: return (unsigned long)_stack_top_cpu3;   /* NEW */
        default: return 0;
    }
}
```

**Reuse vs new, precisely:**
| Region | Source |
|---|---|
| `_stack_top` (primary, 64KB) | EXISTING `linker.ld:43-47` |
| `_stack_top_cpu1` (16KB) | EXISTING MC-2.0 `linker.ld:49-56` — REUSED |
| `_stack_top_cpu2`, `_stack_top_cpu3` (16KB each) | **NEW in MC-2.1** |

---

## 4. THE CERT (cert-first) — `[mc2-smp-equiv]`

### 4.1 Claim

On bare-metal aarch64 under QEMU virt `-smp 4`, the synthetic matmul computed via the bare-metal `pk_parallel_rows` is **byte-identical** to the single-core serial loop, across worker counts `nw ∈ {1, 2, 4}`, under `-O1 -ffp-contract=off`. Marked `[live]` on QEMU TCG; the *barrier/SMPEN* sub-claim is `[live]` only on RPi3 (§4.4, MC-2.2).

### 4.2 The harness — a BOOT SELF-TEST (extend `MC2_SMP_SELFTEST`)

The bare-metal kernel cannot run a host process, so the test lives *in the kernel*, invoked from `main.c` exactly like the existing `MC2_SMP_SELFTEST` block (`main.c:109-150`). MC-2.1 extends that block (or adds a parallel `MC2_EQUIV_SELFTEST` define) to:

1. **Synthetic, fixed-seed, gate-exceeding operands in static BSS.** `out=2048, in=512 → 1048576 MACs > 524288` (the gate, `pk_parallel.h:65`). `W[2048*512]` and `x[512]` as `float`, filled by a deterministic xorshift PRNG (copy `mc0_test.c:70-82`'s `rng_seed`/`rng_u32`/`rng_f`), fixed seed → byte-reproducible across runs and worker counts. Static BSS keeps it off every core's stack (the 16KB worker stacks can't hold it).
2. **The serial reference body.** The matmul body is the UNMODIFIED `dt_linear` shape (`dtr.c:148-157`): for each output row `m`, `acc = b? : 0; for n: acc += W[m*in+n]*x[n]; y[m]=acc;`. The body is parameterized as a `pk_row_body` over `[i0,i1)` (the seam's contract, `pk_parallel.h:39`).
3. Compute `y_serial` by `nw=1` (the inline serial path — `pk_parallel_rows` with the fallback, or a direct `body(ctx,0,out)`).
4. For `nw ∈ {2, 4}` compute `y_par` via the bare-metal `pk_parallel_rows` with the worker count forced (a bare-metal analogue of `pk_parallel_set_threads`, `pk_parallel.c:135` — a static `g_force_nw` the self-test sets).
5. **Assert `memcmp(y_serial, y_par, out*sizeof(float)) == 0`** — byte-for-byte, NOT allclose — AND an **FNV-1a hash** (idiom from `student_shell.c:691-694`) of the output buffer equal across all `nw`.
6. Emit the verdict over PL011 UART (`main.c:44-52` `print()`, `print_long()` `:57-72` for the first-mismatch index), then fall through to the normal T-Kernel boot (so `[mc2-boot-survives]` still holds): print **`MC2-EQUIV: PASS`** or **`MC2-EQUIV: FAIL <nw> @<idx>`**.

Pseudocode (sits inside the `#ifdef MC2_SMP_SELFTEST` block, after the existing `[mc2-boot-survives]` check):
```c
mc2_smp_release_n(4);                    /* wake cores 1,2,3 */
build_synthetic_Wx(g_W, g_x, /*seed*/0xA53C0DE5u);   /* fixed-seed BSS */
mc2_equiv_serial(g_W, g_x, g_y_serial);  /* nw=1 inline */
uint64_t h0 = fnv1a(g_y_serial, OUT);
int ok = 1; int badnw = 0; long badidx = -1;
for (int nw = 2; nw <= 4; nw += 2) {      /* {2,4} */
    pk_smp_force_nw(nw);
    pk_parallel_rows(OUT, mc2_equiv_body, &g_ctx);    /* y -> g_y_par */
    long idx = first_mismatch(g_y_serial, g_y_par, OUT);
    if (idx != -1 || fnv1a(g_y_par, OUT) != h0) { ok = 0; badnw = nw; badidx = idx; break; }
}
if (ok) print("MC2-EQUIV: PASS\r\n");
else { print("MC2-EQUIV: FAIL nw="); print_long(badnw); print(" @"); print_long(badidx); print("\r\n"); }
```

### 4.3 The grep harness — `tests/aarch64/run_mc2_equiv.sh`

Modeled byte-for-byte on `run_mc2_boot.sh` (`tests/aarch64/run_mc2_boot.sh:22-94`):
- `make clean && make EXTRA_CFLAGS=-DMC2_SMP_SELFTEST` (or `-DMC2_EQUIV_SELFTEST` if split).
- Boot under `QEMU_SMP_FLAGS` (`-smp 4`, `Makefile:367-371`) with a `timeout` (the kernel never self-exits — it falls into the T-Kernel idle loop, so the wall-clock timeout is the expected terminator, exactly as `run_mc2_boot.sh:34-50`).
- `grep -q "MC2-EQUIV: PASS"` → exit 0; else fail with the captured UART.
- Add a Makefile target `mc2-equiv-test: ../../tests/aarch64/run_mc2_equiv.sh` next to `mc2-test` (`Makefile:390-391`).

### 4.4 FALSIFIABILITY (mandatory) — and the HONEST QEMU-masks-the-race boundary

The cert is **vacuous unless a known-wrong variant FAILS it.** MC-2.1 ships a debug-only falsifier (a build define, e.g. `-DMC2_EQUIV_RACY` and/or `-DMC2_EQUIV_SMPEN_OFF`) that MUST produce a `memcmp` mismatch. Two distinct falsifier teeth, with sharply different reliability:

**Tooth A — the PARTITION/ARITHMETIC falsifier (full teeth on QEMU, like MC-0):**
A variant that REASSOCIATES the reduction or uses a WRONG slice range — e.g. the worker computes `pk_slice_bm(out, nw, slot, …)` but with a deliberately off-by-one `i1` (overlap/gap), or the MC-0-style strided-partial-sum reassociation (`mc0_test.c:131-160`). This is **pure arithmetic** — it differs from serial *regardless of memory ordering*, so **QEMU TCG WILL catch it.** This proves O1/O2/O3 (the partition + the FP determinism). *This is the tooth that always bites and that mk_pino's automated harness can rely on.*

**Tooth B — the BARRIER/SMPEN falsifier (teeth only `[live]` on real hardware):**
A variant that omits the publish `dsb ish` (step 3) and/or the join `dmb ld` (step 10), OR sets `SMPEN=0` (skip `mc2_set_smpen`, or define `MC2_EQUIV_SMPEN_OFF`). On real weakly-ordered hardware (RPi3 Cortex-A53) this MUST corrupt (stale/torn output) → `memcmp` mismatch. **BUT — the honest caveat (parent plan §4.1, §8 risk 2):**

> **QEMU TCG models memory strongly and may MASK the missing-barrier / SMPEN=0 race.** A TCG run can show `MC2-EQUIV: PASS` *even with the barriers removed*, because TCG does not reproduce the store-buffer / non-coherent-cache reordering a physical A53 exhibits. **So Tooth B's mismatch is only RELIABLY observable `[live]` on real RPi3 (MC-2.2).**

**Therefore the plan states precisely what each platform proves:**

| Sub-claim | Proven by QEMU `-smp 4` PASS? | Proven by RPi3 `[live]`? |
|---|---|---|
| O1/O2/O3 — partition arithmetic, disjoint/total slices, FP left-fold determinism | **YES** — Tooth A bites; byte-identity across nw∈{1,2,4} is real | yes |
| O4 — the barrier/SMPEN discipline makes cross-core reads correct | **NO** (may be masked) — a green QEMU run does NOT mean "barriers verified" | **ONLY here** — Tooth B bites |

This is the salty-bug lesson (the host can't see the device's bug) applied to memory ordering. **MC-2.1 must NOT claim "barriers verified" from a QEMU PASS.** The cert text and the commit message say: *"`[mc2-smp-equiv]` PASS on QEMU proves the worker mechanism + partition arithmetic + FP determinism; the barrier teeth are `[live]`-DEFERRED to MC-2.2/RPi3."*

**Acceptance for MC-2.1a (what makes the cert non-paper on QEMU):**
1. `MC2-EQUIV: PASS` (memcmp==0 + hash equal) for nw∈{1,2,4} under `-smp 4`.
2. **Tooth A falsifier FAILS** (`MC2-EQUIV: FAIL`) under `-smp 4` — proves the cert is not vacuous on the arithmetic.
3. `[mc2-boot-survives]` (the MC-2.0 cert, `run_mc2_boot.sh`) still PASSES — the equiv self-test did not break the boot/scheduler survival.
4. Tooth B is BUILT and RUN; if it happens to FAIL on QEMU that is a bonus, but a PASS is recorded as **"masked, expected — deferred to RPi3"**, NOT as a cert failure.

### 4.5 The idle assertion (carried from MC-0 `[mc0-idle]`)

Between matmuls the secondaries must be in `wfe`, not busy-spinning (§5). MC-2.1 adds a bare-metal wake-counter analogous to `pk_parallel_wake_count` (`pk_parallel.c:140-143`): each worker increments a per-core counter only when it drains a real job (after `gen` advances). The self-test: after the equiv runs, snapshot the counters, do nothing for a bounded spin, re-snapshot — the counters MUST NOT advance (no spurious drains). Print `MC2-IDLE: PASS/FAIL`. (This is MC-2.1b — see §6.)

---

## 5. Idle / overhead / when-NOT discipline (native to the ISA, carried from MC-0/§5 of the parent)

1. **Workers BLOCK on `wfe`, never busy-spin** when the queue is empty (§3.2 loop, `while (g_wq.gen == seen) wfe`). Mirrors the hosted condvar futex-sleep (`pk_parallel.c:76-77`) and the primary dispatcher's own `wfe`-when-idle (`cpu_support.S:120-124`). Between matmuls the secondaries draw ~zero power. Asserted by §4.5.
2. **The gate keeps tiny matmuls serial.** Today's only bare-metal matmul (`dt_linear`, 48×48) is below the 524288-MAC gate; the seam runs it INLINE on the primary (`pk_parallel.c:171-180` fallback shape, mirrored bare-metal) — no secondary touched, byte-identical. The secondaries stay asleep for the entire shipped baby workload. MC-2.1's matmul is **synthetic** and exists only in the self-test.
3. **The `sev` is global** (§1.5 of the parent): waking the workers also spuriously wakes the primary's idle `wfe` — harmless, the dispatcher re-checks `knl_schedtsk` and re-idles. **No dispatcher change.**

---

## 6. Sequencing + honest scope

**MC-2.1a — N-core work-queue + the SYNTHETIC byte-identity cert on QEMU. (THE SMALLEST REAL SLICE.)**
- `linker.ld`: add `_stack_top_cpu2`, `_stack_top_cpu3` (§3.5).
- `mc2_smp.c`: add `mpidr_aff0()` + de-hardcode the worker slot (§3.2); add `struct pk_wq g_wq` + the §3.3 dispatch/worker protocol; add `pk_slice_bm` (verbatim copy of `pk_parallel.c:57-63`); add `mc2_smp_release_n()` + `pk_stack_top_for()` (§3.1); add the bare-metal `pk_parallel_rows` backend (§3.4); add `pk_smp_force_nw()`.
- `main.c`: extend the `MC2_SMP_SELFTEST` block with the synthetic gate-exceeding matmul, `y_serial` vs `y_par` for nw∈{1,2,4}, `memcmp` + FNV, `MC2-EQUIV: PASS/FAIL` (§4.2).
- `tests/aarch64/run_mc2_equiv.sh` + `Makefile` `mc2-equiv-test` target (§4.3).
- **Cert `[mc2-smp-equiv]`:** acceptance §4.4 items 1–3 + Tooth A falsifier. *Smallest real slice = the synthetic matmul self-test that wakes cores 1,2,3, runs nw∈{1,2,4}, and proves memcmp==0 + Tooth-A-fails under `-smp 4`, with `[mc2-boot-survives]` still green.*

**MC-2.1b — falsifier hardening + idle assertion.**
- Add the Tooth B (`-DMC2_EQUIV_RACY` / `-DMC2_EQUIV_SMPEN_OFF`) variants and record the honest QEMU-masks result (§4.4).
- Add the bare-metal wake-counter + `MC2-IDLE: PASS/FAIL` (§4.5).
- Add the `pk_slice_bm` self-consistency unit check (§3.4 stronger guard).

**MC-2.2 — RPi3 hardware `[live]` (DEFERRED — needs physical hardware).**
- Spin-table release (BCM2837 mailboxes, not PSCI — `arch_reboot.c:6`); the SAME worker loop + barriers.
- **The ONLY place Tooth B (the barrier/SMPEN falsifier) is fully load-bearing.** Netboot on-ramp: `make tftp`, `make run-rpi3` (`Makefile:397-398,417-425`).

**DEFERRED / OUT-OF-SCOPE (explicit):**
- **Wiring a REAL bare-metal matmul (`dt_linear`) to the seam** — deferred to a later wave; only pays off when SS-7 grows R3 past the 524288-MAC gate (§0, §3.4). MC-2.1 does NOT modify `dtr.c`.
- **x86 AP-startup (INIT-SIPI-SIPI)** — a separate bigger lift; MC-2.1 is aarch64-ONLY (parent §6). x86 keeps the inline-serial fallback.
- **Any T-Kernel scheduler change** (`knl_ctxtsk`/`knl_schedtsk`, `cpu_support.S` dispatch) — that is ② (§7).
- **Splitting the contraction dim** across workers — reassociates the reduction, breaks byte-identity. Only output-row partitioning ships (O1).

---

## 7. ② carry-over (building on parent plan §3.4) — what MC-2.1 hands ②, and the boundary it still holds

**What MC-2.1's N-core work-queue hands to ② full SMP:**

| MC-2.1 artifact | ② full-SMP reuse |
|---|---|
| N-core PSCI release loop `mc2_smp_release_n()` + per-slot stacks (§3.1, §3.5) | ② needs the identical N-core bringup to make N secondaries *schedulable* |
| Per-CPU block `g_cpu[]` with MPIDR-derived slot (§3.2) | becomes per-CPU `knl_ctxtsk`/`knl_schedtsk` + run-queue, indexed by the same MPIDR Aff0 |
| The ONE work-queue lock (`ldaxr/stlxr` + `stlr`) + the `done` counter discipline (§3.3) | the template for the task-table / run-queue locks ② must add |
| The §3.3 `ish`-barrier + SMPEN discipline | survives unchanged; ② tasks doing matmul still hit the same `[mc2-smp-equiv]` byte-identity cert |
| `pk_parallel_rows` fork-join + `wfe`-idle/`sev`-wake contract (§3.4) | the wake/idle-halt machinery a real scheduler needs (the dispatcher's `.Lidle` `wfe`, `cpu_support.S:120-124`, becomes per-CPU) |

**The safety boundary MC-2.1 still holds (② must add these; MC-2.1 deliberately does NOT):**
- **Workers NEVER touch `knl_ctxtsk`/`knl_schedtsk`.** The §3.2 worker loop reads only `g_wq` and `g_cpu[]`; it never reads a scheduler symbol (verifiable by grep on the new TU).
- **No per-CPU run-queue.** Each secondary runs the ONE fixed work-queue drain (§3.2), never picks a task.
- **No task migration, no cross-CPU `knl_ctxtsk` locking.** The single `knl_ctxtsk` stays primary-only, single-threaded.
- **The dispatcher in `cpu_support.S` stays BYTE-FOR-BYTE UNTOUCHED** (`cpu_support.S:48-124`, incl. `.Lidle` `:120-124`). MC-2.1 adds only `start.S` landing-pad reuse (already shipped in MC-2.0) and the new `mc2_smp.c` work-queue.
- **No IPIs (SGI).** Wake is `sev`/`wfe` + shared flags; the absent `GICD_SGIR` path (parent §1.4) is an ② lift.

② is exactly the work of turning the single `knl_ctxtsk` into per-CPU scheduling **on top of** this bringup + work-queue. **MC-2.1 stops one step short of the scheduler — that boundary IS the safety margin.**

---

## 8. OPEN RISKS (honest)

1. **The barrier discipline is the #1 risk, and QEMU can't fully prove it (§4.4).** A missing `dsb ish`/`dmb`, or `SMPEN=0`, is a silent shared-memory race → split mind. The §3.3 discipline closes it; Tooth B proves it load-bearing — but **only `[live]` on RPi3**. Conservatism: audit reviews every barrier line-by-line against the §3.3 table; do NOT credit a QEMU PASS as "barriers verified."
2. **`pk_slice_bm` drift (§3.4).** The bare-metal partition is a hand-copy of `pk_parallel.c:57-63` (can't link the hosted TU). If it drifts, bits diverge silently. Mitigation: the self-test's `nw=1` path uses the SAME `pk_slice_bm`, and MC-2.1b adds an explicit self-consistency check.
3. **The worker-slot hardcode (§1.6).** MC-2.0's `g_cpu[1]` hardcode (`mc2_smp.c:158`) MUST be replaced by the MPIDR-derived slot or core 2/3 will both stomp slot 1. This is the single most error-prone diff; the cert catches it (a stomped slot → wrong/missing `done` → join timeout or memcmp mismatch).
4. **PSCI CPU_ON for cores 2,3 on QEMU virt.** Assumed identical to core 1 (`mc2_smp.c:231` Aff0=index). If a core fails to wake, the bounded join (§3.3 step 9, REUSE `mc2_smp.c:249-266`) times out → `MC2-EQUIV: FAIL`, NOT a hang. Verify each `g_cpu[slot].woken==1` in the self-test.
5. **The synthetic-matmul reality (§0).** MC-2.1 delivers NO speedup to the shipped baby — the only bare-metal matmul is below the gate, and the cert matmul is synthetic. Its value is the deterministic-worker mechanism + the ② bringup foundation. Do not oversell.
6. **Stack sizing.** 16KB per worker (§3.5) holds a leaf matmul body; the synthetic `W`/`x`/`y` live in static BSS (§4.2), NOT on the stack. If a future real body recurses, revisit. The cert would crash (garbage output → memcmp fail) if a stack overflowed, so it is self-guarding.

---

### Appendix — grounding (file:line, all on commit 488d4232)

- **Parent plan:** `docs/architecture/mc2-baremetal-smp-plan.md` §2 (invariant), §3.1 (seam), §3.2 (per-CPU + worker), §3.3 (lock + barrier table — the heart), §4.1 (the cert + falsifier + QEMU-masks caveat), §7 (MC-2.1 bullet).
- **MC-2.0 impl built ON:**
  - per-CPU block + reserved slots 2/3: `arch/aarch64/mc2_smp.c:38-60`; offset macros + asserts `:46-55`.
  - the ldaxr/stlxr lock: `mc2_smp.c:110-129`; SMPEN set `:137-144`, asm `start.S:247-249`.
  - `pk_smp_worker_loop` (slot-1 HARDCODE at `:158`): `mc2_smp.c:151-185`.
  - PSCI CPU_ON release: `psci_cpu_on` `mc2_smp.c:198-213`, `mc2_smp_release_one` `:218-237`; function id `0xC4000003` `:192`.
  - bounded-join watchdog (`MAX_TRIES`): `mc2_smp.c:249-266`.
  - `#ifdef MC2_FAULTING_TILE` variant: `mc2_smp.c:102-106`; faulting tile path `:166-172`.
  - landing pad / EL1 setup: `start.S:172-264` (`_secondary_worker` `:173`, `_secondary_el1_setup` `:220`); shared `el1_vectors` defined `cpu_support.S:211-212`.
  - `-smp 4` targets: `boot/aarch64/Makefile` `QEMU_SMP_FLAGS` `:367-371`, `run-smp` `:375-378`, `run-smp-fault` `:383-386`, `mc2-test` `:390-391`.
  - `MC2_SMP_SELFTEST` hook → `MC2-BOOT` verdict: `boot/aarch64/main.c:27-35,109-150`; `print`/`print_long` `:44-52,57-72`.
  - the grep harness PATTERN: `tests/aarch64/run_mc2_boot.sh` (build+boot+grep `:52-94`).
  - the secondary stack region MC-2.0 added: `boot/aarch64/linker.ld:49-56` (`_stack_top_cpu1`).
- **The shipped seam MC-2.1 must match bit-for-bit:** `arch/common/llm/pk_parallel.c:57-63` (`pk_slice`, ragged remainder on LAST slice); `pk_parallel.h:9-16` (invariant), `:104` (`pk_parallel_rows`), `:45` (`PK_PARALLEL_MIN_ROWS 64`), `:65` (`PK_PARALLEL_MIN_MACS 524288`); hosted fallback shape `pk_parallel.c:171-180`; gate logic `:225-241`; wake counter `:140-143`.
- **The MC-0 cert model (byte-identity + FNV + killable falsifier):** `tests/llm/mc0_test.c` (FNV `:115-122`, the reassociating falsifier `:131-160`, the equiv loop `:194-219`, the falsifier "MUST DIFFER" assertion `:221-250`); `tests/llm/run_mc0.sh:24-30` (build line). FNV idiom source `arch/common/llm/student_shell.c:691-694`.
- **The bare-metal matmul that DOES exist (below gate, NOT wired):** `arch/common/dtr.c:148-157` (`dt_linear`); R3 dims `arch/common/r3_incontext.c:81-88`. Confirmed zero `pk_parallel` refs in `dtr.c`/`moe.c`/`r3_incontext.c`.
- **Build flags (one mind, one math):** `boot/aarch64/Makefile:63-66` (`-O1 -ffp-contract=off -mstrict-align`).

---

**Note:** the design-harden agent may have had Write denied in its sandbox; if so the commander persists this verbatim from the agent's returned plan. **Awaiting MC-2.0 merge + commander review + a separate impl→audit cycle (MC-2.1a first — the smallest real slice).**
