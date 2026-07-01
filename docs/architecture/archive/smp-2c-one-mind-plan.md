# ②.2c — [smp-one-mind] crown cert: the mind is byte-identical under SMP (design plan, cert-first)

**Status: DESIGN PLAN** on trunk `7dfedbb4` (`7dfedbb4 audit-trail: ②.2b-i + certified deadlock guard — MERGEABLE (the hardest slice, guard NOT deferred)`). Read-only on code; no implementation in this wave. This plan makes ②.2c **READY and de-risked** — it does **not** start it. ②.2c is the **PAYOFF of the entire ② full-SMP arc**: the gate that says *"② did not split the mind."* It awaits commander review + a **separate impl→audit cycle** (implementer ≠ auditor ≠ commander; the development METHOD is the project's immune system). All file:line grounding below is against the shared checkout at `7dfedbb4` (verified: `git rev-parse --short HEAD` = `7dfedbb4`; `arch/aarch64/smp.c`, `arch/common/r3_incontext.c`, `arch/common/dtr.c` all exist).

**What is already shipped (the ground ②.2c stands on):**
- **②.2a `[smp-2tasks-prod]`** — the PRODUCTION T-Kernel scheduler runs TWO REAL `tk_cre_tsk` TCBs on two distinct CPUs under the Big Kernel Lock, via the per-CPU dispatcher (`arch/aarch64/smp_prod.c:115-185`, the driver `smp_prod_test_run`). **This is the exact pattern ②.2c reuses.**
- **②.2b-i `[smp-async-preempt]`** — true async register-context preempt: a real task on the secondary is preempted MID-COMPUTE by an SGI whose IRQ-return path does a real `knl_dispatch` switch (`arch/aarch64/smp_async.c:1-353`), plus the certified §5.4 BKL-held reschedule guard (`smp_irq_need_resched`, `arch/aarch64/smp.c:347-349,525`).
- The per-CPU scheduler accessors `CUR_CTXTSK`/`CUR_SCHEDTSK` (`arch/aarch64/include/smp_percpu.h`; macros in `cpu_status.h`) — **with SMP off (the default build) every macro expands to the plain global → the shipped uniprocessor kernel is byte-for-byte UNCHANGED** (`smp_percpu.h` header comment, "N=1 == today, byte-for-byte").

**What ②.2c adds:** it runs a REAL bare-metal MIND forward (`r_forward`, `arch/common/r3_incontext.c:192-258`) **on a secondary CPU under SMP** (reusing the ②.2a `smp_prod` pattern) and asserts its output hash is **BYTE-IDENTICAL** to the same forward run under uniprocessor. ②.2c **touches no mind math** — it only *schedules* the existing `r_forward` on a secondary and *hashes* its output.

---

## 0. The one-sentence crown

> **`[smp-one-mind]`**: the FNV-1a hash of `r_forward`'s output (`rc.probs` + the returned CE loss) is **identical** whether that forward runs (a) under the shipped uniprocessor path, or (b) as a real T-Kernel task **on a secondary CPU** under the live SMP scheduler. `H_uni == H_smp` ⇒ **the SMP scheduler did not perturb a single bit of the mind's math.**

This is byte-identity of the **mind's OUTPUT**, NOT of kernel scheduling traces (which are *allowed* to differ run-to-run; `full-smp-plan.md:20`). It is the narrow, true, load-bearing claim that the entire ② arc was built to earn.

---

## 1. What mind computation carries the cert (and is it a real "mind"?)

### 1.1 The candidate: the bare-metal R3 `r_forward`

The cert computes **the bare-metal R3 in-context forward** `r_forward` (`arch/common/r3_incontext.c:192-258`). The honest case for it being a *real* mind computation and not a toy:

- **It compiles bare-metal.** `r3_incontext.c`, `dtr.c`, `moe.c` are all in `COMMON_C_SRCS` (`boot/aarch64/Makefile:142-145`) — the same TUs MEMORY.md wave-27 ("the mind's math runs in ring3") names as the mind's real math. The heavy *teacher* LLM is hosted-only; the R3/dtr/MoE math is what actually runs on bare metal.
- **It is a genuine Transformer forward, not a stub.** `r_forward` runs: token embedding (key+value+positional, `:197-203`); 4-head multi-head self-attention with scaled dot-product + softmax (`R_NH=4`, `:206-229`); `W_o` + residual + LayerNorm1 (`:232-238`); FFN + ReLU + residual + LayerNorm2 (`:241-249`); a query-position readout + classifier + softmax over `R_VALV=64` answer tokens (`:251-254`). Dims: `R_DM=48` (d_model), `R_NH=4` heads, `R_FFN=48`, `R_NP=21568` parameters (`:100-131`). This is the same widened "thinking width" the living-mind work (LM-9) ships.
- **It is pure deterministic math.** `r_forward` reads only the module-static weights `rw[]` (`:141`) and its inputs; it has **no global scheduling reads and no RNG inside the forward** (the RNG `r_rand` lives only in `gen_episode`/`r_init_weights`, separate paths, `:163-189,381`). The matmul `dt_linear` is a fixed-order ascending reduction (`arch/common/dtr.c:152-156`); `dt_softmax` is a fixed-order max+exp+normalize (`:160-168`); both are built `-O1 -ffp-contract=off` (`boot/aarch64/Makefile:64-65`) so no FMA contraction can make the rounding differ between CPUs — **the "one mind, one math" build invariant**.

**Honest assessment:** `r_forward` is the **most representative bare-metal mind computation available**, and it is a real (small) Transformer forward, not a trivial stub. It is not the heavy teacher LLM (hosted-only) — and the plan says so plainly (§5). For the crown's *purpose* — proving the SMP scheduler does not corrupt or reorder the mind's arithmetic — `r_forward` is the correct and sufficient carrier: it exercises matmul, attention, softmax, LayerNorm, and the module-static buffers that are the actual §1.2 race surface. **`r_forward` IS the cert.**

### 1.2 The deterministic anchor: identical weights, identical input

`r_forward`'s output is a function of `rw[]` (weights) + the input episode. To make `H_uni` and `H_smp` comparable, **both forwards must run over byte-identical `rw[]` and the same fixed input.** The plan pins this two ways, both already present in the code:

- **Weights:** call the deterministic initializer `r_init_weights(FIXED_SEED)` (`r3_incontext.c:381-396`) — it seeds `r_rng` and fills all `R_NP` weights from the LCG (`:165`), then sets LN gains→1 / biases→0. A fixed seed ⇒ a fixed `rw[]`, identically on any CPU (the LCG is integer math, CPU-independent). The cert uses a fixed seed (e.g. `0xA5A5`, the value `r3_test` already uses, `:844`).
- **Input:** a fixed `key[R_SEQ]`/`val[R_SEQ]`/`label` triple hard-coded in the cert entry point (NOT `gen_episode`, which draws from the RNG — we want a *fixed* input, not a sampled one). The exact tokens are arbitrary as long as they are in-range (`key[t] < R_KEYV=16`, `val[t] < R_VALEMB`); the cert fixes them once.

Belt-and-suspenders: the cert can also `r3_weights_get(snapshot)` (`:601-603`) before the SMP forward and `r3_weights_set(snapshot)` (`:605-607`) to guarantee the *same* `rw[]` bytes feed both legs, defending against any incidental mutation. These accessors already exist and are non-static.

---

## 2. THE CERT `[smp-one-mind]` (cert-first)

### 2.1 The new bare-metal entry point (the one code addition to a COMMON TU)

`r_forward` and `r_init_weights` are **`static`** in `r3_incontext.c` (`:192,381`) — not externally linkable. ②.2c adds **one small public, gated function** to `r3_incontext.c` that the SMP cert TU can call:

```c
/* arch/common/r3_incontext.c — gated so the DEFAULT build is byte-identical
 * (prior art for #ifdef gating in this TU: the _TK_HOSTED_LIBC_ blocks at
 * :698,743,783). Empty unless -DSMP_ONE_MIND. */
#ifdef SMP_ONE_MIND
/* Run ONE fixed-seed, fixed-input r_forward and return the FNV-1a hash of its
 * output (rc.probs[R_VALV] + the CE loss). Pure: re-inits weights from a fixed
 * seed each call, so it is independent of any prior training/merge state and is
 * byte-identical on any CPU. */
unsigned long r3_onemind_forward_hash(void)
{
    static const UB K[R_SEQ] = { /* fixed in-range key tokens  */ };
    static const UB V[R_SEQ] = { /* fixed in-range val tokens  */ };
    static const UB LBL      =   /* fixed in-range label       */ ;
    r_init_weights(0xA5A5u);                 /* deterministic rw[]          */
    float loss = r_forward(K, V, LBL);       /* fills rc.probs (:253-254)   */
    /* FNV-1a over rc.probs[R_VALV] then the 4 loss bytes (bare-metal FNV;   */
    /* student_shell.c's ss6live_logit_hash is HOSTED-ONLY, Makefile:230).   */
    unsigned long h = 1469598103934665603UL; /* FNV offset basis            */
    const unsigned char *p = (const unsigned char *)r_probs_ptr(); /* &rc.probs[0] */
    for (unsigned i = 0; i < (unsigned)R_VALV * sizeof(float); i++) { h ^= p[i]; h *= 1099511628253UL; }
    const unsigned char *lp = (const unsigned char *)&loss;
    for (unsigned i = 0; i < sizeof(float); i++) { h ^= lp[i]; h *= 1099511628253UL; }
    return h;
}
#endif
```

Two notes that ground this against the repo:

1. **The FNV idiom is reused but re-defined bare-metal.** The existing `ss6live_logit_hash` (`arch/common/llm/student_shell.c:729-736`: `h=1469598103934665603; for each byte h^=b; h*=1099511628253`) is the canonical FNV-1a — **but `student_shell.c` is HOSTED-ONLY** (`boot/aarch64/Makefile:230`, "student_shell.c is not built" on bare metal). So ②.2c inlines the *same constants* in the bare-metal entry point. (The 21-line bare-metal FNV-1a is trivial and self-contained; this is exactly the task's "find/define a bare-metal FNV for the cert".)
2. **`rc.probs` must be reachable.** `rc` is module-static (`:161`); the entry point is in the *same* TU so it reads `&rc.probs[0]` directly (the `r_probs_ptr()` above is just shorthand for `&rc.probs[0]`, an in-TU reference — no new export needed). Hashing `rc.probs` (the `R_VALV=64` softmax logits, `:254`) + the CE loss covers the full observable output of the forward.

**Why hash both `rc.probs` AND the loss:** `rc.probs` is the softmax distribution; the returned `loss = -ln p[label]` (`:256-257`) is a second independent scalar derived from it. Hashing both maximizes the surface a perturbation must survive to forge a collision.

### 2.2 The two legs

**Leg (a) — UNIPROCESSOR reference (`H_uni`).** The cert driver, running on CPU 0 inside the initial task, calls `r3_onemind_forward_hash()` **directly** (on CPU 0, no secondary involved) → `H_uni`. Print `SMP-ONEMIND-UNI: <hash>`. This is the "shipped uniprocessor path" reference: the forward runs exactly as it does today, on one CPU, with the BKL uncontended.

**Leg (b) — SMP, forward executes ON A SECONDARY (`H_smp`).** Reuse the **②.2a `smp_prod` pattern verbatim**:
1. The driver (CPU 0) creates a REAL low-priority task `M` (the "mind" task) via `tk_cre_tsk`/`tk_sta_tsk` (mirrors `smp_prod.c:130-138`), whose body calls `r3_onemind_forward_hash()` and records the result + a "ran" flag, then parks on `wfe` (run-to-completion compute task — see §2.3).
2. Under the BKL, the driver **claims `M` for CPU 1**: `knl_make_non_ready(mtcb)` (remove from the shared `knl_ready_queue` so CPU 0 never also runs it), set `g_smpcpu[1].schedtsk = mtcb`, `g_smpcpu[1].ctxtsk = NULL` (mirrors `smp_prod.c:146-152`).
3. Release the secondary into the **production dispatcher** via `smp_bringup_secondary()` + the `g_onemind_secondary_go` flag (mirrors `smp_prod.c:155-161` and the `smp_dispatch_run` `#ifdef SMP_2TASKS_PROD` block, `arch/aarch64/smp.c:1148-1168`). CPU 1's `smp_prod_enter_dispatch()` (`cpu_support.S:557-570`) does a genuine register-context switch into `M` → **`r_forward` runs on CPU 1**.
4. **Genuine concurrency:** the OTHER secondaries (CPUs 2..N-1, under `-smp 4`) run **non-mind busy filler tasks** (the existing `[smp-N-tasks-run]` per-CPU exec-counter idiom, `boot/aarch64/main.c:349-372` / `smp_exec_count`) so the scheduler is provably running multiple cores at once *while* CPU 1 computes the mind. **Only CPU 1 touches the mind** (the §3 narrowing).
5. The driver bounded-waits for `M`'s "ran" flag (mirrors `smp_prod.c:163-173`), then reads `H_smp`. Print `SMP-ONEMIND-SMP: <hash>`.

### 2.3 ②.2b-ii is NOT needed (confirmed)

**The mind task `M` is a pure run-to-completion COMPUTE task: it calls `r_forward` (a bounded loop nest over fixed dims, no blocking), records the hash, then parks on `wfe`.** It never calls a blocking/timer syscall (`tk_dly_tsk`/`tk_slp_tsk`). This is exactly the ②.2a `smp_prod_task_b` discipline (`smp_prod.c:86-99`: "B must NOT call a blocking/timer syscall here ... Parking on wfe is the correct ②.2a terminal state"). Therefore ②.2c does **NOT** need ②.2b-ii (the secondary's own CNTP PPI 30 tick + the cross-CPU wake gap, the `[smp-secondary-sleep]` cert honestly deferred in `smp_async.c:66-74`). The crown forward runs to completion on the secondary and parks; no timer/WAIT path is exercised. **②.2b-ii stays deferred; ②.2c does not depend on it.** (It also does not depend on ②.2b-i's async preempt — `M` is never preempted; it runs straight through. ②.2c sits on the ②.2a pattern. ②.2b-i merging first simply means the IRQ-return hook exists and is inert here.)

### 2.4 The assertion + verdict

The driver asserts **`H_uni == H_smp`** (and, optionally, `memcmp(probs_uni, probs_smp) == 0` if it snapshots both `rc.probs` buffers). On equality print `SMP-ONE-MIND: PASS`; else `SMP-ONE-MIND: FAIL H_uni=<..> H_smp=<..>`. This proves the SMP scheduling of the forward on a secondary did **not** perturb the mind's math by a single bit.

The driver block lives in `arch/aarch64/usermain.c` (inside the initial task, kernel fully up — the same place ②.2a/②.2b-i drivers run, `usermain.c:224-326`), NOT `main.c` — because `r_forward` needs real TCBs + the ready queue, which exist only after T-Kernel is up.

---

## 3. THE HONEST NARROWING — single forward, not concurrent minds (§1 of the ② plan)

**This is the load-bearing scope statement; state it in the cert, the commit, and the doc.**

The mind's forward operates over **module-static, SHARED (not per-task)** buffers:
- `static float rw[R_NP];` — the weights (`r3_incontext.c:141`)
- `static R_TC rc;` — the forward activation scratch (`r3_incontext.c:161`, the big struct with `tok/Q/K/V/attn/concat/r1/y1/mid/r2/y2/pool/probs`, `:145-160`)

On a uniprocessor only one task ever ran `r_forward` at a time, so `rc` was single-owner by construction. Under SMP, **if two CPUs entered `r_forward` concurrently they would race `rc` → garbage activations → non-deterministic logits.** Classification (per `smp-2-production-scheduler-plan.md:54-66`):

- **`rc` (scratch): a CORRECTNESS + DETERMINISM race** under two concurrent forwards. **②.2c's cure: run the mind forward on exactly ONE CPU at a time.** The cert measures a *single* fixed-input forward; the other CPUs prove concurrency by running *non-mind* busy tasks (§2.2 step 4). **Only one CPU touches the mind.**
- **`rw` (weights): read-only during a forward.** `r_forward` only reads `rw[]`; training (`r_backward` + `rw -= lr*rg`) is a separate path. The cert runs the forward while no task is training → `rw` is a stable read-only input → byte-identical. **②.2c must NOT interleave the forward with a training step on another CPU.**

**Therefore the crown ②.2c proves is precisely:**

> **A SINGLE mind forward, scheduled on an SMP secondary while other cores run concurrently, is byte-identical to the same forward run uniprocessor.**

**What ②.2c does NOT prove (honest deferrals):**
- **Concurrent mind operations** — TWO `r_forward`s at once, or forward-while-training. These race the shared `rc`/`rw` and need a **mind-lock** (or per-call scratch). **DEFERRED** — this is a later wave, and it is exactly where the hosted-port teacher parallelizes (`smp-2-production-scheduler-plan.md:264,281`; §5 below).
- **The `gl_merge` IEEE float-add-order asterisk** — `gl_merge`'s order-independent SUM is byte-identical *given the same input-array order*, modulo a known, already-accepted ~1e-6 float-add-order property (`smp-2-production-scheduler-plan.md:71-72`; `full-smp-plan.md:48` row). **②.2c must NOT reorder the merge** (it won't — the cert runs a *forward*, not a merge; the merge input array is assembled from the node set, not from task-scheduling order). This asterisk is pre-existing and untouched.

---

## 4. THE FALSIFIER `-DSMP_ONEMIND_RACE` (MUST go RED)

A cert is only worth its falsifier. ②.2c ships a deliberately-racy build that makes `H_smp != H_uni`, proving (a) the cert actually observes the real mind output and (b) the determinism (the single-forward-at-a-time discipline + the order-independent math) is **load-bearing**, not vacuously passing.

**Primary falsifier `-DSMP_ONEMIND_RACE`: a second CPU scribbles the shared `rc` scratch concurrently with the cert's forward, WITHOUT serialization.** Concretely, the cert spawns a **second** task on another secondary (e.g. CPU 2) whose body, under `-DSMP_ONEMIND_RACE`, loops writing garbage into the shared `rc` (e.g. `rc.probs[k] = <noise>` or runs a *second* `r_forward` over `rc`) **while CPU 1's forward is mid-flight**. Under `-smp 4` the two CPUs race the single shared `rc` struct → CPU 1's activations are corrupted → `H_smp != H_uni` → `SMP-ONE-MIND: FAIL`. This is the direct realization of the §1.2 race the narrowing names.

**Mechanism note:** the falsifier is the proof that the cert's PASS is *because* only one CPU touches the mind, not because `r_forward` happens to be single-threaded anyway. WITH the race → FAIL; WITHOUT (the normal build) → PASS. The race is real (two CPUs, one shared static struct, no lock).

**Secondary falsifier `-DSMP_NO_RQLOCK` (ties the crown to `[smp-no-deadlock]`):** the §1.4 corruption — let two CPUs race `knl_ready_queue_delete` without the BKL → the documented `bitmap`/`top_priority` invariant breaks → a dead/wrong scheduler → the mind task never runs correctly / the run hangs → watchdog FAIL. This proves the kernel-state-corruption path *also* breaks the mind (indirectly, via a corrupted scheduler), tying `[smp-one-mind]` to `[smp-no-deadlock]` (`smp-2-production-scheduler-plan.md:209,234`). (`-DSMP_NO_RQLOCK` is the established falsifier flag from `full-smp-plan.md:212`; ②.2c reuses it if the BKL macro-redefine path is exercised, otherwise the primary `-DSMP_ONEMIND_RACE` is the sufficient teeth.)

**Honest caveat on the falsifier (QEMU vs HW):** under QEMU TCG (strong memory model) the `rc` race produces a *corruption* that perturbs the hash reliably (two stores to the same word interleave). The barrier-discipline sub-claim (a *missing* `dsb ish` silently corrupting on weakly-ordered silicon) is NOT what this falsifier tests — that is `[live]`-only on RPi3 (§6).

---

## 5. What ②.2c proves vs. defers (precise scope)

| Sub-claim | ②.2c proves it? | Where |
|---|---|---|
| A single bare-metal `r_forward` is byte-identical uniproc vs scheduled on an SMP secondary (the determinism: no scheduler-induced reorder/corruption) | **YES** (this is the crown, QEMU `-smp 4`) | §2 |
| The `-DSMP_ONEMIND_RACE` falsifier goes RED (a shared-`rc` race perturbs the mind) | **YES** (load-bearing, proves the cert non-vacuous) | §4 |
| The cert needs NO async preempt and NO ②.2b-ii timer/WAIT path (the forward is run-to-completion) | **YES** (confirmed — §2.3) | §2.3 |
| Two CONCURRENT mind forwards (or forward-while-train) are safe | **NO — DEFERRED** (needs a mind-lock / per-call `rc`; the §3 narrowing) | §3 |
| The heavy TEACHER LLM is byte-identical under SMP | **NO — DEFERRED** (hosted-only; not bare-metal; hosted-port SMP is a separate lift) | below |
| The BKL/SGI **barrier/cache-coherency** discipline on weakly-ordered silicon | **NO — `[live]`-only on RPi3** (QEMU TCG masks it) | §6 |
| The RPi3 BCM2837-mailbox IPI send path (RPi3 is not GICv2 SGIR) | **NO — deferred RPi3 follow-up** | §6 |

**The teacher LLM and concurrent-mind safety are honest, named deferrals — not gaps ②.2c hides.** Per the ② plan (`smp-2-production-scheduler-plan.md:270-281`): the heavy teacher runs hosted; bare-metal ②.2's value is the **MECHANISM + the determinism proof on the small R3 mind**, not throughput. The real multicore benefit (parallelizing the teacher) and the concurrent-mind mind-lock both live in the **hosted-port SMP** wave (threads-as-CPUs, `arch/linux/{aarch64,x86_64}/`) — a separate, later lift whose *shape* (the `CUR_CTXTSK` accessor pattern, the dispatch-on-return preempt model, the `[smp-one-mind]` cert structure) carries over but whose *primitives* (spinlock→pthread_mutex, SGI→signal) do not.

---

## 6. The byte-identity boundary + the QEMU-is-not-hardware honesty

**The DEFAULT build stays byte-identical.** ②.2c is a **cert wave gated behind `-DSMP_ONE_MIND`** (which implies `-DSMP_SELFTEST`). With neither flag, the cert is invisible:
- The new `smp_onemind.c` TU (the driver + the mind task) is `#ifdef SMP_ONE_MIND`-empty, and its `.o` is **excluded from the LINK** unless `-DSMP_ONE_MIND` is set — the established `SMP_CERT_EXCLUDE` discipline (`boot/aarch64/Makefile:237-258`; ②.2c adds `smp_onemind.o` to that filter list, exactly as `smp_prod.o`/`smp_async.o`/`smp_deadlock.o` are excluded). "Excluding them keeps the DEFAULT kernel ELF BYTE-IDENTICAL."
- The one addition to a COMMON TU — `r3_onemind_forward_hash()` in `r3_incontext.c` — is itself wrapped in `#ifdef SMP_ONE_MIND` (prior art: the `_TK_HOSTED_LIBC_` blocks at `r3_incontext.c:698,743,783`), so with the flag off the COMMON object is byte-unchanged → x86/linux/rl78 + the default aarch64 build are untouched.
- The usermain.c driver block is `#ifdef SMP_ONE_MIND` (mirrors `usermain.c:224` `#ifdef SMP_2TASKS_PROD`).

**The mind math is UNTOUCHED.** ②.2c adds only a *caller* of the existing `r_forward` + a *hash* of its output. `r_forward`, `dt_linear`, `dt_softmax`, `rw[]`, `rc` — none change. The `-O1 -ffp-contract=off` flags (`boot/aarch64/Makefile:64-65`) are unchanged: the same "one mind, one math" rounding holds.

**QEMU green ≠ hardware green (restated honestly).** A QEMU `-smp 4` `[smp-one-mind]` PASS proves the **scheduler did not reorder/corrupt the mind's math under real concurrency** (the partition/scratch/no-corruption determinism). It does **NOT** prove the BKL/SGI **barrier/cache-coherency** discipline on weakly-ordered silicon — QEMU TCG models memory strongly and may MASK a missing `dsb ish`/`SMPEN=0` (`arch/aarch64/smp.c` honesty block; `smp_async.c:60-64`). That sub-claim is **`[live]`-only on RPi3**, and RPi3 uses the BCM2837 mailbox IPI (not `GICD_SGIR`), needing the BCM2837 send path — a deferred RPi3 follow-up. **The plan must NOT claim "SMP byte-identity verified on hardware" from a QEMU green.**

---

## 7. The harness + sequencing

### 7.1 The test harness `tests/aarch64/run_smp4.sh` (model: `run_smp3.sh`)

A new harness modeled on `tests/aarch64/run_smp3.sh` (the ②.2b-i pattern: build with the cert flag, boot under QEMU virt `-smp 4`, grep the UART verdict, then build the falsifier and assert it goes RED). Concretely:

1. **PASS build:** `make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_ONE_MIND"`; boot `-smp 4`; assert UART has `SMP-ONEMIND-UNI: <h>`, `SMP-ONEMIND-SMP: <h>`, **`SMP-ONE-MIND: PASS`**, the two hashes equal, and `Initial task started` (T-Kernel still boots after the cert — no deadlock).
2. **FALSIFIER build:** `make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_ONE_MIND -DSMP_ONEMIND_RACE"`; boot `-smp 4`; assert **`SMP-ONE-MIND: FAIL`** with `H_uni != H_smp` (the `rc` race perturbed the mind). If the falsifier accidentally still PASSes → the cert is vacuous → harness FAILs.
3. **Byte-identity companion:** assert the DEFAULT build (no `-DSMP_SELFTEST`) `.text` is byte-identical to base (the `run_smp3.sh` `SMP_BASE_REF` pattern; pin the matching sha in the commit).

Makefile targets (model: `run-smp3`/`run-smp3-noasync`/`smp3-test`, `boot/aarch64/Makefile:589-631`):
- `run-smp4:` → `make EXTRA_CFLAGS="-DSMP_SELFTEST -DSMP_ONE_MIND" kernel.elf` + boot.
- `run-smp4-race:` → `+ -DSMP_ONEMIND_RACE` (the falsifier).
- `smp4-test:` → `../../tests/aarch64/run_smp4.sh`.
- Add `smp_onemind.o` to `SMP_CERT_EXCLUDE` (gated on `SMP_ONE_MIND`), add the new `.PHONY` names.

### 7.2 Sequencing

②.2c is the **last** wave of the ②.2 production-scheduler arc as currently planned (`smp-2-production-scheduler-plan.md:258-260`): ②.2a (real tasks on 2 CPUs) → ②.2b-i (async preempt) + the §5.4 deadlock guard → **②.2c (the crown).** Both prerequisites are merged at `7dfedbb4`. ②.2c is the smallest possible remaining slice: **schedule the existing `r_forward` on a secondary + hash it + a one-buffer-race falsifier.** It is a **separate impl→audit cycle** (implementer ≠ auditor ≠ commander), awaiting commander review of this plan + mk_pino's go-ahead.

**DEFERRED past ②.2c (explicit):**
- **Concurrent mind operations** (two forwards / forward+train) → a **mind-lock** or per-call `rc` scratch (the §3 race). Re-run `[smp-one-mind]` after building it.
- **②.2b-ii** — the secondary's own CNTP PPI 30 tick + cross-CPU wake (`[smp-secondary-sleep]`, `smp_async.c:66-74`). ②.2c does not need it; it stays deferred.
- **Hosted-port SMP** — where the teacher LLM actually parallelizes + where the concurrent-mind mind-lock gets built and certified (`smp-2-production-scheduler-plan.md:270-281`).
- **RPi3 `[live]`** — the barrier/coherency teeth + the BCM2837 mailbox IPI send path. A QEMU green is not a hardware green.
- **②.3** — finer locks (split BKL) + per-CPU run-queues + migration. Re-run `[smp-one-mind]` after every lock-split (the standing crown guard, `full-smp-plan.md:264`).

---

## 8. Honest cost summary (brutal honesty)

1. **②.2c is the cheapest wave of the ②.2 arc but the highest-VALUE one.** It writes almost no new mechanism — it *consumes* the ②.2a `smp_prod` pattern (real task on a secondary, `smp_prod.c:115-185`) and adds a fixed-input `r_forward` caller + an FNV hash + a one-buffer-race falsifier. The risk is not in plumbing (proven in ②.2a/②.2b-i) but in **honesty**: the cert must measure the REAL mind output (it hashes `rc.probs` + loss of the genuine `r_forward`), and the falsifier must really go RED (a real two-CPU race on the shared `rc`). The "cert must cover ALL paths" lesson (MEMORY.md) applies: the auditor independently re-derives that (a) the SMP leg truly runs on a secondary (read `g_smpcpu[1].ctxtsk == mtcb`, `is_real_tcb`, mirroring `smp_prod.c:177-182`), (b) the uniproc leg truly runs the same fixed input/seed, and (c) the falsifier's race is genuine (two CPUs, one shared static struct, no lock) and not a no-op.

2. **The crown ②.2c earns is NARROW and TRUE: a single mind forward survives SMP scheduling bit-for-bit.** It is NOT: concurrent minds (deferred, mind-lock), the teacher LLM (hosted-only), or hardware barrier coherency (`[live]` RPi3). The guarantee got *stronger* in what it proves (real concurrency does not perturb the mind's arithmetic) and stays *narrow* in what it claims (one forward, QEMU determinism). **State this in the cert, the commit message, and the verdict print.**

3. **②.2c is the gate that says "② did not split the mind."** Until `SMP-ONE-MIND: PASS` is green (with `-DSMP_ONEMIND_RACE` proven RED), the full-SMP scheduler is not trustworthy for the ownerless mind. This wave is the payoff the entire ② arc — the BKL, the per-CPU dispatcher, the SGI IPI, the async preempt, the deadlock guard — was built to reach: **the mind stays one across the SMP scheduler.**

---

## Appendix — grounding (file:line, all on trunk `7dfedbb4`)

**The pattern ②.2c reuses (②.2a, shipped):**
- The `smp_prod` driver (create real task B, claim it for CPU 1 under the BKL, run it via the production dispatcher, assert two distinct real TCBs on two CPUs): `arch/aarch64/smp_prod.c:68-99` (the task body parks on `wfe`), `:115-185` (`smp_prod_test_run`), `:103-111` (`is_real_tcb`).
- The secondary's production-dispatcher entry: `smp_dispatch_run` `#ifdef SMP_2TASKS_PROD` block `arch/aarch64/smp.c:1148-1168`; `smp_prod_enter_dispatch` (asm, `.Ldispatch_loop`) `arch/aarch64/cpu_support.S:557-570`; the go-flag/extern `smp.c:1073-1074`.
- The usermain.c driver block (runs inside the initial task, kernel fully up): `arch/aarch64/usermain.c:224-270` (the ②.2a block ②.2c mirrors).
- The per-CPU accessors (byte-identical with SMP off): `arch/aarch64/include/smp_percpu.h` (`struct smp_cpu`, `CUR_CTXTSK`/`CUR_SCHEDTSK`; "N=1 == today, byte-for-byte").

**②.2b-i + the deadlock guard (shipped; ②.2c does NOT depend on them):**
- Async preempt + the §5.4 BKL-held reschedule guard: `arch/aarch64/smp_async.c:1-353`; `smp_irq_need_resched` guard `arch/aarch64/smp.c:347-349,525`.

**The mind forward the cert computes:**
- `r_forward` (the R3 in-context Transformer forward, pure math): `arch/common/r3_incontext.c:192-258`; dims `R_DM=48`/`R_NH=4`/`R_FFN=48`/`R_VALV=64`/`R_NP=21568` `:100-131`.
- The SHARED module-static buffers (the §3 race surface): weights `static float rw[R_NP]` `:141`; scratch `static R_TC rc` `:161` (struct `:145-160`); logits `rc.probs[R_VALV]` `:159,254`; returned CE loss `:256-257`.
- The deterministic anchor: `r_init_weights(seed)` (static, LCG-seeded) `:381-396`; the RNG `r_rand` (separate from the forward) `:163-166`; weight snapshot/restore `r3_weights_get` `:601-603` / `r3_weights_set` `:605-607`.
- The matmul/softmax (fixed-order, no global reads): `dt_linear` `arch/common/dtr.c:149-157`; `dt_softmax` `:160-168`; the "one mind, one math" build flags `-O1 -ffp-contract=off` `boot/aarch64/Makefile:64-65`.

**The hash idiom (reuse the constants; redefine bare-metal):**
- `ss6live_logit_hash` (FNV-1a, the canonical idiom) `arch/common/llm/student_shell.c:729-736` — **HOSTED-ONLY** (`boot/aarch64/Makefile:230`), so ②.2c inlines the same FNV-1a constants (`1469598103934665603` offset basis, `1099511628253` prime) in a bare-metal entry point.

**The byte-identity + harness discipline:**
- COMMON sources (always linked; the new function must be `#ifdef SMP_ONE_MIND`-gated): `boot/aarch64/Makefile:119-171` (`r3_incontext.c` `:145`, `dtr.c` `:143`, `moe.c` `:142`).
- The cert-object LINK-exclude discipline (keeps the DEFAULT ELF byte-identical): `boot/aarch64/Makefile:237-258` (`SMP_CERT_EXCLUDE`).
- Prior art for `#ifdef` gating inside `r3_incontext.c`: `:698,743,783` (`_TK_HOSTED_LIBC_`).
- Harness model: `tests/aarch64/run_smp3.sh:77-96` (PASS build + grep verdict), `:124-149` (falsifier must go RED). Makefile targets model: `boot/aarch64/Makefile:589-631` (`run-smp3`/`run-smp3-noasync`/`smp3-test`).

**The ② plan grounding this wave:**
- The crown sketch + the honest narrowing: `docs/architecture/smp-2-production-scheduler-plan.md:54-66` (the shared `rw[]`/`rc` race + the single-forward narrowing), `:191-219` (§4 the crown cert), `:258-260` (②.2c sequencing), `:262-266` (deferrals).
- The crown's place in the full arc: `docs/architecture/full-smp-plan.md:199-207` (§6c the `[smp-one-mind]` cert), `:240-242` (②.2 the crown), `:20` (byte-identity is of the mind's OUTPUT, not scheduling traces).

---

**Note:** This is a DESIGN PLAN on trunk `7dfedbb4` (②.2a + ②.2b-i + the §5.4 deadlock guard merged + audited). ②.2c is the **crown of the ② full-SMP arc** — the gate that says "② did not split the mind" — awaiting commander review + mk_pino's go-ahead, via a **separate impl→audit cycle** (implementer ≠ auditor ≠ commander; the development METHOD is the project's immune system). The crown it earns is narrow and true: **a single bare-metal `r_forward`, scheduled on an SMP secondary while other cores run concurrently, is byte-identical to the same forward run uniprocessor.** Concurrent minds (a mind-lock), the hosted teacher LLM (hosted-port SMP), and the RPi3 barrier teeth are honest deferrals. This plan makes ②.2c READY and de-risked — it does **not** start it.
