# Multi-core compute (③) — deterministic parallel matmul: implementation plan (cert-first, stepping-stone to full SMP)

**Status: DESIGN PLAN** by an automated design-harden pass on trunk `7df9ba96` (`7df9ba96 integrate SS-6-live audit-trail row`). Awaiting commander review + a separate impl→audit cycle. Read-only on code; no implementation in this wave.

mk_pino's decision: **do ③ now** (parallelize the math, keep the kernel uniprocessor). The north star remains **② full SMP scheduler**; ③ is designed as a deliberate **stepping-stone** — the worker / per-CPU / work-queue abstractions introduced here are the foundation ② will reuse.

---

## 0. The crown jewel this plan must not break

"One mind" = FedAvg / Fisher / Path-W² averaging of `rw[]` across the fleet (`mw_fold_region` / `gl_merge`, per memory). It works **only** because every node computes a forward **byte-identically**. The salty bug (wave-49) proved that a single rounding-order divergence (clang FMA contraction vs gcc) silently splits the fleet's shared mind; the fix was `-ffp-contract=off` on **all** targets — "one mind, one math."

The project already reasons explicitly in these terms. `arch/common/llm/student.c:696`:

> `── canonical reduction: sum the per-expert [D] outputs weighted by the router softmax, in ASCENDING slot order j (identical to the single-node forward's accumulation order -> byte-for-byte equal, -O1 -ffp-contract=off, both arches). NO reassociation.`

A naive parallel reduction **reassociates** the floating-point accumulation → different bits → broken byte-identity → split mind. **This is the central design problem, and it is the whole reason the GPU backend is DEFERRED** (see §6). The single non-negotiable invariant of ③:

> **The parallel matmul produces a result BYTE-IDENTICAL to the serial loop, for ANY worker count (1, 2, 4, 8) and ANY completion order.**

This plan's entire claim is that **CPU multi-core can hit true byte-identity where the GPU cannot**, because we never reassociate — we only split *which core runs which already-serial dot-product*.

---

## 1. The deterministic-parallel strategy (the load-bearing idea)

### 1.1 The matmul is already embarrassingly parallel by OUTPUT row

Every heavy matmul in the codebase has the identical shape `y[i] = Σ_j W[i][j] · x[j]`, with the **outer loop over output index `i`** and a **self-contained inner accumulation** per `i`:

| Site | file:line | loop |
|------|-----------|------|
| Q8_0 dequant-matmul (teacher) | `arch/common/llm/quant.c:62` | `for i<out { acc=0; for b,k acc += d*q[k]*x[...]; y[i]=acc }` |
| Q4_0 dequant-matmul (teacher) | `arch/common/llm/quant.c:94` | same shape, `y[i]=acc` |
| F32 dense (teacher) | `arch/common/llm/forward.c:168` | `for i<out { acc=0; for j<in acc+=row[j]*x[j]; y[i]=acc }` |
| `dt_linear` (R3 / MoE / dtr) | `arch/common/dtr.c:149` | `for m<M { s=b?b[m]:0; for n<N s+=W[m*N+n]*x[n]; y[m]=s }` |
| student MoE expert | `arch/common/llm/student.c:690` | `for i { acc=0; for h<DFF acc+=w2r[h]*eh[h]; eo[i]=acc }` |

**Key property:** each output element `y[i]` is the result of ONE serial inner loop, written by ONE store. No two output elements share an accumulator. The contraction (inner `j`/`b`/`k`/`h`) is **never** split across workers.

### 1.2 The strategy: PARTITION BY OUTPUT, never by the contraction dim

Split the **output index space `[0, out)`** into disjoint contiguous (or fixed strided) ranges, one per worker. Worker `w` computes `y[i]` for its assigned `i`, running **the exact same inner loop, in the exact same order, with the exact same left-fold accumulation** as the serial path. There is:

- **no cross-thread reduction of a single dot-product** — the dangerous case;
- **no shared accumulator** — each `y[i]` is touched by exactly one worker;
- **no reassociation** — the inner `acc += …` order is byte-for-byte the serial order.

Therefore `y[i]` is bit-identical to serial **for every `i`, independent of how `[0,out)` was partitioned or which worker finished first.** Partitioning is a pure *scheduling* decision over independent stores; it cannot change a single bit.

This is strictly stronger than the GPU path (§6): the GPU shader tree-reduces the contraction, which DOES reassociate, which is exactly why GPU output is fenced out of `rw[]` merges (`gpu-3-wiring.md:45`). **CPU row-partitioning has no such fence** — its output is admissible on the training/merge path too, because it is literally the same bits.

### 1.3 The one rule the implementation must obey

> **Never split the inner (contraction) loop across workers. Only split the outer output-index loop. A worker owns a closed set of output indices and runs the unmodified serial inner loop for each.**

If a future optimization ever needs to split the contraction (very long `in`), the ONLY admissible form is a **fixed-order tree reduction** with a partition **independent of worker count and completion order** (e.g. fixed power-of-two tile boundaries summed in fixed index order). That changes the bits vs the current left-fold, so it would require re-blessing the golden logit — **out of scope for ③; flagged as an OPEN RISK**, not a Phase. For all matmuls in the current code, `out`/`M` is large enough that output partitioning alone suffices.

---

## 2. Survey of the real compute — where parallel actually pays, and where it does NOT

### 2.1 The teacher (SmolLM2-135M) — the REAL beneficiary

`arch/common/llm/forward.c:381-446`: per layer, 7 `matmul_tensor` calls (attn q/k/v/out, ffn gate/up/down) plus the output head over the full vocab (`forward.c:446`, `tok_embd` `[d_model, vocab]`). Dims are read from GGUF (`forward.h:41-67`), 135M params. For SmolLM2-135M these are roughly `d_model=576`, `d_ff=1536`, `vocab≈49152`, `n_layer=30`. The **output head** alone is `out≈49152` rows × `in=576` ≈ 28M MACs — partitioning `[0, 49152)` across 8 cores is a clean ~8× of the dominant cost. Every ffn matmul (`out=1536`) and the output head are comfortably above any reasonable size threshold. **This is the node where multi-core matmul earns its keep.**

### 2.2 The byte student — TINY, do NOT parallelize today (honest)

`student.h:57,78`: M-tier `d_model=128`, `dff=128`, 4 layers, 256-byte vocab; L-tier `d=256/dff=512` (`student_shell.c:680`). The hottest student matmul is an expert FFN of `out≤256`, `in≤512` ≈ 128K MACs at the very top tier, far less in the M-tier baby. At `d=128`, an expert row-matmul is ~16K MACs — **thread dispatch + join latency (microseconds) exceeds the compute (sub-microsecond).** Parallelizing the current baby would be *slower*. ③ must **gate on size** and leave the small student serial. SS-7 (a bigger student) is a future beneficiary, not today's.

### 2.3 `dt_linear` (R3 / MoE / dtr) — the merge-critical path; correctness first

`arch/common/dtr.c:149` is the single choke point for **31 call sites** in `arch/common/*.c` (R3 in-context, MoE routing, dtr training/DMN distillation). Today the shipped R3 matmul is **48×48 = 2304 MACs** (`gpu-3-wiring.md:41`) — **below threshold, stays serial.** But `dt_linear` is the path that writes `rw[]`, so when a bigger student (SS-7) crosses threshold here, byte-identity is **mandatory, not optional** (this is the path the GPU is forbidden on). Because output-partitioning is bit-exact, the parallel `dt_linear` is admissible here — **this is the structural advantage over GPU-3 and the reason ③ is worth doing.**

### 2.4 Honest size threshold

Order-of-magnitude, pending the MC-1 measurement (§4):

- **Parallelize when `out · in ≳ 2^18 (≈262144 MACs)`** — the same gate the GPU design chose (`gpu-3-wiring.md:32`, `DT_GPU_MIN_MACS≈262144`). Reuse the constant name family for one mental model.
- Below that, the per-tile dispatch/join overhead dominates; **stay serial.**
- Net effect today: **teacher ffn + output head parallelize; R3 48×48, the M-tier baby, q/k/v projections of small models stay serial.** The gate must be a runtime check on `out·in`, applied inside the choke points, NOT a compile flag.

---

## 3. The worker abstraction — phased, and engineered as ②'s foundation

### 3.1 The seam: one function, two backends

Introduce a single dispatch primitive that BOTH phases implement and BOTH choke points (`qz_matmul_*`/`matmul_tensor`, `dt_linear`) call:

```
/* Run body(w, i0, i1) over a partition of [0, out) across <=NW workers,
 * then JOIN. body() computes y[i] for i in [i0,i1) using the UNMODIFIED
 * serial inner loop. Deterministic: partition is a pure function of
 * (out, NW); completion order is irrelevant because outputs are disjoint. */
void pk_parallel_rows(size_t out, void (*body)(void *ctx, size_t i0, size_t i1),
                      void *ctx);
```

The matmul bodies become: serial inner loop, parameterized by `[i0,i1)`. When `out·in < THRESHOLD` (or `NW==1`, or no pool), `pk_parallel_rows` calls `body(ctx, 0, out)` inline — **identical bits, zero overhead, the serial path.** This keeps the bare-metal/RL78 and the no-pool case trivially correct.

### 3.2 Phase A — pthread worker pool (Linux / Android: the TODAY win)

- A **bounded, math-only** pool of `NW-1` pthreads created **once** at first parallel matmul (`get_nprocs`/`sysconf(_SC_NPROCESSORS_ONLN)` for `NW`, capped). The dispatching kernel thread participates as worker 0.
- **This ADDS pthreads where arch/linux has zero today** (confirmed: `selfc_proc.c:13` comment "verified: zero pthread_create in arch/linux"; the only match in the tree is that comment). These are **NOT scheduler threads** — they never touch `knl_ctxtsk`/`knl_schedtsk`, never run T-Kernel tasks, only drain matmul tiles. The T-Kernel scheduler stays single-threaded and unchanged.
- Synchronization: classic fork-join. A tiny `struct { body, ctx, i0, i1, done }` per worker; a condvar to wake on a new job, a barrier/counter to join. The kernel thread BLOCKS in the join (it is doing a synchronous matmul anyway), so there is no concurrency with the T-Kernel scheduler during the matmul.
- **Idle discipline (interacts with the just-fixed idle path, `wave-idle-yield`):** the scheduler idles via `sched_yield` (`x86_64/cpu_support.S:156-160`, aarch64 `cpu_support.S:130` "idle is sched_yield"). The matmul workers MUST NOT busy-spin between jobs — when the queue is empty they **block on the condvar** (futex sleep), not spin. Between forwards the pool is fully asleep and consumes no CPU, mirroring the scheduler's yield-when-idle contract. A spin-wait here would fight the OS scheduler and burn a phone battery — explicitly forbidden.

### 3.3 Phase B — bare-metal "constrained SMP" matmul workers (mk_pino's real goal)

No pthreads on bare metal. Today the aarch64 secondary cores are **parked**: `arch/aarch64/start.S:40-43` silences them, `:144 _secondary_park: wfe`. Phase B **wakes them as dedicated deterministic matmul workers**, NOT as schedulable CPUs:

- **Bringup:** release the secondary cores from `_secondary_park`. On aarch64 QEMU virt / RPi3, use the existing parking convention — set a per-CPU `release_addr`/work-queue pointer and `sev` to wake them from `wfe` (RPi3 spin-table), or PSCI `CPU_ON` where a PSCI provider exists. Each secondary gets its **own stack** and runs a fixed **tile-loop**, never the T-Kernel dispatcher (`arch/aarch64/cpu_support.S`, the `.Ldispatch_loop` for the primary stays untouched).
- **The worker loop:** `while (1) { tile = wq_pop(); if (!tile) wfe; else run_tile(tile); signal_done(tile); }`. One global **work-queue with one spinlock**. The primary pushes tiles, `sev`s the workers, then joins on a done-counter. Workers `wfe` (not busy-spin) when the queue is empty — same idle discipline as Phase A, native to the ISA.
- **Determinism is unchanged:** the worker still runs the unmodified serial inner loop over its output rows; partition is `(out, NW)`-pure. Bit-identity holds identically to Phase A — the cert (§4) runs against both.
- **This is the bigger lift** (cache coherency setup, secondary MMU/EL config, per-CPU GIC for any IPI, the spin-table/PSCI handshake). Flagged as such; MC-2.

### 3.4 What carries over to Phase C — ② full SMP (explicit)

The whole point of doing B carefully is that ② reuses it. What carries over:

| ③ artifact | ② reuse |
|------------|---------|
| Secondary-core **bringup** (release from park, per-CPU stack, EL/MMU init) | ② needs identical bringup to make secondaries *schedulable* |
| **Per-CPU data** block (stack, id, current-job slot) | becomes per-CPU `knl_ctxtsk`/`knl_schedtsk` + run-queue |
| The **one work-queue lock** (locking discipline, acquire/release order) | the template for the task-table / run-queue locks ② must add |
| `pk_parallel_rows` **fork-join + idle-block** contract | the IPI/wake + idle-halt machinery a real scheduler needs |
| The **`-ffp-contract=off` + byte-identity** discipline | survives unchanged; ② tasks doing matmul still hit the same cert |

What does NOT carry (and must be flagged at the ②→ boundary): ③ has **no task migration, no per-CPU run-queue, no cross-CPU `knl_ctxtsk` locking** — the T-Kernel scheduler stays single-threaded. ② is exactly the work of turning the single `knl_ctxtsk` into per-CPU scheduling on top of this bringup. ③ deliberately stops short of touching the scheduler; that boundary is the safety margin.

---

## 4. THE CERT (cert-first)

### 4.1 `[par-matmul-equiv]` — byte-identity (the gate)

**Claim:** for the production matmul choke points, the parallel result is **byte-identical** to the serial result, for worker counts ∈ {1, 2, 4, 8}, under `-O1 -ffp-contract=off`.

**Harness (`[in-proc]`, single process, N threads):**
1. Build a real matmul: random but fixed-seed `W` (Q8_0 and F32) and `x` of a teacher-scale shape (`out≈49152, in=576` for the head; `out=1536, in=576` for ffn). Same generator the existing llm tests use (`tests/llm/`).
2. Compute `y_serial` via the unmodified serial path (force `NW=1` / inline-`body` branch).
3. For `NW ∈ {2,4,8}` compute `y_par` via `pk_parallel_rows`.
4. Assert `memcmp(y_serial, y_par, out*sizeof(float)) == 0` **byte-for-byte** (not allclose). Also assert a logit **hash** (reuse the FNV-1a over the logits buffer already at `student_shell.c:687`) matches across all `NW` and matches the existing golden.
5. Extend to a **real forward** hash: full teacher forward with the pool ON at each `NW` must equal the golden GPU-off logit hash (the `0xe2391516`-class fpdet golden referenced in `gpu-3-wiring.md:27`).

**FALSIFIABILITY (mandatory — the cert must be *killable*):** add a debug-only **non-deterministic reduction** variant that reassociates — e.g. a contraction split that accumulates partials in **worker-completion order** (or simply sums the row in `NW` strided partial-sums and adds them in finish order). Run the SAME harness against it. It **MUST FAIL** `memcmp` (different rounding order → different bits). If the falsification variant *passes*, the cert is vacuous and the harness is rejected. This is the wave-49 lesson encoded: prove the test can see the bug it exists to catch. (Mirrors the SS-6 "byte-identity proven by responder-sabotage" discipline at HEAD `7df9ba96`.)

**Can MC-0's byte-identity cert pass on the ACTUAL matmul code? — YES, by construction.** Because `pk_parallel_rows` with output-partitioning never reorders any inner accumulation, `y_par[i]` executes the *identical* float operations in the *identical* order as `y_serial[i]`. The only way it could differ is a real bug (overlapping partitions, a worker writing the wrong range, a stray FMA from a mis-set build flag) — which is exactly what the cert is there to catch. The cert is expected to PASS on a correct implementation and FAIL on the sabotage variant.

### 4.2 `[par-matmul-speedup]` — honest measurement (`[in-proc]`)

- Wall-clock a full teacher forward at `NW ∈ {1,2,4,8}` on a real multi-core host (and `[live]` on a phone for Phase A in MC-1). Report speedup and efficiency.
- Report the **size threshold sweep**: matmul wall-time serial vs `NW=4` across `out·in` from 2^12 to 2^22; find the empirical crossover where parallel wins. Publish it; set the runtime gate constant from the measured crossover (expected near the `262144` MAC figure, §2.4). **State the regime where parallel is SLOWER and confirm the gate keeps the small student serial.**
- Honest determinism cost: output-partitioning forgoes **load-balancing reassociation tricks** (e.g. atomic global accumulators) that a non-deterministic matmul would use. Quantify any imbalance cost when `out` is not divisible by `NW` (last worker does the remainder) — expected negligible for large `out`.

### 4.3 Marking

- MC-0, MC-1 host: `[in-proc]` (single process, N threads). MC-1 phone: add `[live]`. MC-2 bare-metal: `[live]` on QEMU virt + RPi3.
- The byte-identity cert is **the gate**; the speedup cert is **honest reporting** and must never be used to justify lowering the equivalence bar.

---

## 5. Honest overhead, when NOT to parallelize, idle interaction

1. **Tiny matmuls lose.** The M-tier baby (`d=128`) and R3 (48×48) are below threshold; dispatch/join overhead exceeds compute. The gate **must** keep them serial. Today's *shipped* mind (student baby + R3) sees **no benefit** from ③ — ③ pays off on the **teacher** and on **future bigger students (SS-7)**. State this plainly; do not oversell.
2. **Sync cost.** Fork-join per matmul has fixed cost (condvar wake / `sev` + barrier). For the teacher's ~210 matmuls/forward this is amortized; for one tiny matmul it is pure loss. Hence the per-call size gate, not a global switch.
3. **Determinism vs speedup tradeoff (the honest cost).** Refusing to reassociate means we cannot use a single global atomic accumulator or completion-order reduction (which would balance load better on ragged shapes). For output-partitioned matmuls this costs essentially nothing (outputs are independent); the cost would only bite if we ever split the contraction — which §1.3 forbids in ③. So in ③ the determinism cost is **near-zero**; the tradeoff becomes real only at the ②/contraction-split frontier (OPEN RISK).
4. **Idle path (just-fixed `wave-idle-yield`).** Workers MUST block (condvar futex sleep on Linux; `wfe` on bare metal) when no tiles are queued — **never busy-spin.** This matches the scheduler's `sched_yield`/`wfe`-when-idle contract (`x86_64/cpu_support.S:156`, `aarch64/cpu_support.S:130`). Between forwards the pool is fully asleep: zero idle CPU, zero battery drain on a phone. A spinning worker pool would regress exactly the idle behavior just fixed — explicitly forbidden and covered by an idle-CPU assertion in MC-1.

---

## 6. The GPU precedent — why CPU multi-core is the RIGHT first parallelism

A real Vulkan f32 matmul backend already ships in the APK (`android/app/src/main/cpp/gpu/gpu_vk.c`), but `gpu-3-wiring.md:3` records the decision: **DEFER**. The reasons are instructive and directly motivate ③:

- The GPU shader **tree-reduces the contraction** → reassociated rounding → **NOT byte-identical** to the CPU left-fold (`gpu-3-wiring.md:24`). So GPU output is admissible (if ever) only on a *pure inference* forward and is **fenced out of every `rw[]` merge** (`:45,:57`) — "one mind, one math" by exclusion.
- It is **provably zero speedup today** (largest shipped matmul 48×48 < the GPU threshold; CPU beats this shader even at 4096²) — `:41`.
- Its determinism is handled by **DEFERRAL + a weaker tolerance cert**, not by achieving byte-identity (`:27-31`).

**③ is strictly better on the dimension that matters most to this project:** CPU output-partitioning is **byte-identical**, so it needs **no merge fence** and is admissible on the training/merge path the GPU is forbidden on. The `262144`-MAC size gate is borrowed from the GPU design (`:32`) so both backends share one mental model. If GPU-3 is ever wired, the `pk_parallel_rows` seam and the size gate are shared infrastructure; the GPU simply remains the *non-deterministic* backend confined to inference, while CPU multi-core is the *deterministic* one usable everywhere.

---

## 7. Sequencing — small falsifiable waves

**MC-0 — the deterministic strategy + the `[par-matmul-equiv]` cert harness.** *Smallest real slice:* implement `pk_parallel_rows` (Phase A pthread pool) + re-express ONE choke point (`qz_matmul_q8_0`, `quant.c:54`, the teacher's hot quantized matmul) as `body(i0,i1)`. Ship the byte-identity harness (§4.1) over a real teacher-scale Q8_0 matmul at `NW∈{1,2,4,8}` **and** the sabotage/falsification variant that must FAIL. *Gate:* `memcmp==0` across all `NW`; sabotage FAILS; idle pool consumes ~0 CPU between calls. No forward wired yet — pure equivalence proof. **This slice's byte-identity cert is expected to PASS on the real `quant.c` code** because output-partitioning provably preserves accumulation order.

> **STATUS: SHIPPED (impl) on `wave-mc0-parallel-matmul`, base `03481a52`, awaiting separate audit.**
> - Primitive: `arch/common/llm/pk_parallel.{c,h}` — HOSTED-only pthread pool (`pk_parallel_rows`), partition is a pure function of `(out, NW)` with the ragged remainder on the LAST slice; helpers BLOCK on a condvar when idle (no busy-spin → respects `wave-idle-yield`); `NW=sysconf(_SC_NPROCESSORS_ONLN)` capped to 8, override via `PKERNEL_MATMUL_THREADS` / `pk_parallel_set_threads`. Fallback to inline serial `body(ctx,0,out)` when `NW<=1`, no pool, or `out < PK_PARALLEL_MIN_ROWS` (64). Math-only — never touches `knl_ctxtsk`/`knl_schedtsk`.
> - Re-expression: `qz_matmul_q8_0` (`quant.c`) now runs `qz_q8_body(ctx,i0,i1)` (the UNMODIFIED serial inner loop) over `pk_parallel_rows(out, …)`. Inner contraction NOT split (plan §1.3).
> - Cert: `tests/llm/run_mc0.sh` + `mc0_test.c`. **[par-matmul-equiv]** `memcmp==0` AND FNV-1a hash equal for `NW∈{1,2,4,8}` on out∈{1536, 49152} plus two ragged shapes (out=1530, out=1031 — neither divisible by 4 or 8). **[par-matmul-falsifier]** a reassociating (strided-partial-sum) variant DIFFERS for every NW → cert has teeth. **[mc0-idle]** wake-counter delta=0 and ~0 s CPU across a 300 ms idle gap.
> - Builds: all four (`boot/linux`, `boot/linux_x86_64`, `boot/x86`, `boot/aarch64`). Bare-metal x86/aarch64 use the inline fallback — `quant.c`/`pk_parallel.c` are LLM (hosted) tier and are NOT linked there (they use `student_stub.o`), so bare metal stays serial with zero pthreads.
> - Honest: **no speedup claimed** (that is MC-1, with the `out·in` size gate wired into the forward). MC-0 is the equivalence proof only; the `PK_PARALLEL_MIN_ROWS` row guard is a coarse stand-in until MC-1 measures the `out·in` crossover.

**MC-1 — wire the pthread pool to the teacher forward (the big matmuls).** Route `matmul_tensor` (`forward.c:159`) through `pk_parallel_rows` with the size gate. Cert: full teacher forward logit hash byte-identical to the GPU-off golden at every `NW`; `[par-matmul-speedup]` measured on host and `[live]` on a phone; idle-CPU assertion. Keep the small student + R3 serial (below gate) — assert the gate routes them inline.

**MC-2 — bare-metal constrained-SMP matmul workers.** Wake the parked aarch64 secondaries (`start.S:144`) as tile-loop workers behind the SAME `pk_parallel_rows` seam; one work-queue + one spinlock; `wfe` idle. Cert: same byte-identity harness on QEMU virt and RPi3; `[live]` speedup. The bigger lift — bringup, per-CPU stack, cache coherency.

**Phase C / ② (north star, separate future wave).** Build the per-CPU run-queue and cross-CPU scheduler **on top of MC-2's bringup + per-CPU data + locking discipline**. Out of scope here; ③ exists to make this reachable, not to do it.

---

## 8. DEFERRED / OUT-OF-SCOPE + OPEN RISKS

**Out of scope for ③:**
- Any change to the T-Kernel scheduler (`knl_ctxtsk`/`knl_schedtsk`, `cpu_support.S` dispatch). The kernel stays uniprocessor. (That is ②.)
- Splitting the **contraction** dimension across workers (would reassociate; §1.3). Only output-index partitioning ships in ③.
- GPU-3 wiring (stays DEFERRED per `gpu-3-wiring.md`).
- Parallelizing the current tiny student / R3 (below the size gate by design).

**OPEN RISKS (honest):**
1. **Determinism-vs-speedup at the contraction frontier.** If a future model has `in` so large that the inner loop dominates and output-partitioning under-uses cores, the only deterministic fix is a fixed-order tree reduction — which changes the golden bits and needs a re-bless. Quantify before crossing it.
2. **Bare-metal SMP bringup complexity (MC-2).** Spin-table vs PSCI, secondary MMU/EL/cache-coherency config, GIC per-CPU setup. The classic SMP-bringup minefield; biggest single risk in the plan. Aarch64 first (parked-core convention exists); x86 AP-startup (INIT-SIPI-SIPI) is a separate later lift.
3. **The tiny-student reality.** ③ delivers **nothing** to today's shipped baby mind; its value is the teacher and SS-7. If SS-7 stays small, ③'s payoff stays teacher-only. Do not let "multi-core" read to users as a baby-speedup.
4. **GPU-backend determinism stays unsolved** (by design DEFERRED). ③ does not fix it; ③ is the *deterministic* alternative. If both ever coexist, the merge path must still assert it reads CPU-authoritative `rw[]` (`gpu-3-wiring.md:57`).
5. **Thread-count nondeterminism via `get_nprocs`.** `NW` varies by machine — but byte-identity is **invariant to `NW`** by construction (§1.2), so the golden is machine-independent. The cert proves exactly this (the `NW∈{1,2,4,8}` sweep). The MC-0 harness must include a `NW` not dividing `out` (ragged remainder) to prove partition arithmetic is correct.

---

### Appendix — grounding (file:line)

- Uniprocessor scheduler (single `knl_ctxtsk`/`knl_schedtsk`, `sched_yield` idle): `arch/linux/x86_64/cpu_support.S:118,156-160`; `arch/linux/aarch64/cpu_support.S:127-130`.
- Zero pthreads in arch/linux today: only `arch/linux/selfc_proc.c:13` (a comment).
- Teacher matmuls: `arch/common/llm/forward.c:159` (`matmul_tensor`), called at `:381-446`; `arch/common/llm/quant.c:54` (Q8_0), `:86` (Q4_0).
- `dt_linear` choke point: `arch/common/dtr.c:149`; 31 call sites in `arch/common/*.c`.
- Student dims: `arch/common/llm/student.h:57,78,83` (d=128/dff=128 M-tier); L-tier d=256/dff=512 `arch/common/llm/student_shell.c:680`.
- Byte-identity discipline already in code: `arch/common/llm/student.c:696-705` ("ASCENDING slot order … byte-for-byte equal … NO reassociation").
- Logit hash for the cert: `arch/common/llm/student_shell.c:687` (FNV-1a).
- Parked bare-metal secondaries: `arch/aarch64/start.S:40-43,144`.
- GPU precedent / DEFER / determinism fence: `docs/architecture/gpu-3-wiring.md:3,24,32,41,45,57`; `docs/architecture/gpu-compute.md:177` (`-ffp-contract=off` not on GPU shader).
- Salty-bug "one mind, one math" (`-ffp-contract=off`): per MEMORY.md wave-49.

---
