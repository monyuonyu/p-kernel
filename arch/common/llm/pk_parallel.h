/*
 *  pk_parallel.h — deterministic row-partitioned parallel dispatch (MC-0).
 *
 *  Multi-core compute (③): the matmul choke points partition their OUTPUT
 *  index space [0,out) across a small math-only worker pool, each worker
 *  running the UNMODIFIED serial inner loop for its closed range of output
 *  rows. See docs/architecture/multicore-matmul-plan.md §1, §7.
 *
 *  THE INVARIANT (the "one mind, one math" crown, wave-49):
 *    The result is BYTE-IDENTICAL to the serial loop for ANY worker count
 *    (1,2,4,8) and ANY completion order, because:
 *      - only the outer output-index loop is split (never the contraction);
 *      - each y[i] is written by exactly one worker (no shared accumulator);
 *      - the inner `acc += ...` left-fold order is the serial order, verbatim.
 *    Partitioning is a pure scheduling decision over independent stores; it
 *    cannot change a single bit. (-O1 -ffp-contract=off mandated.)
 *
 *  Tiering: the pthread pool implementation (pk_parallel.c) is HOSTED-ONLY
 *  (Linux / Android — the LLM "身体" tier; never on bare-metal x86/aarch64,
 *  which have no pthreads and where these matmuls are not even compiled).
 *  Bare-metal never references these symbols (the LLM tier is not built there,
 *  the matmul call sites use student_stub.o), so there is no separate inline
 *  fallback in this header; the serial path lives in pk_parallel.c itself
 *  (NW<=1 / out below the row gate → body() runs inline, identical bits).
 */
#ifndef PKERNEL_PK_PARALLEL_H
#define PKERNEL_PK_PARALLEL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The partition body. Computes y[i] for i in [i0,i1) using the unmodified
 * serial inner loop. ctx carries the matmul operands. Must touch only its
 * own output range (disjoint across workers -> no locking, no false sharing
 * of a single y[i]). */
typedef void (*pk_row_body)(void *ctx, size_t i0, size_t i1);

/* Below this many output rows, never spin up workers — dispatch/join cost
 * would exceed the compute. The size gate (out*in) lives at the call site
 * (the matmul knows `in`); this is a cheap secondary guard on `out` alone so
 * a degenerate tiny `out` never pays the join. */
#define PK_PARALLEL_MIN_ROWS 64

/*
 * THE SIZE GATE (MC-1). A matmul is worth parallelizing only when its total
 * work `out * in` (MACs) exceeds dispatch/join overhead. Below this, the
 * choke points run the serial inline body — so the d=128 student expert, the
 * R3 48x48, and the teacher's small q/k/v projections stay SERIAL (they would
 * be SLOWER under the pool; plan §2.2, §2.4, §5).
 *
 * SET FROM THE MC-1 CROSSOVER SWEEP (tests/llm/run_mc1.sh, this sandbox: 4
 * cores online). The GPU design's 262144 (2^18) is too low: at 2^18 the pool
 * still LOSES (~0.4-0.5x) — the fork-join fixed cost (~0.4 ms for NW=4 here)
 * does not amortize until ~2^19. The first power-of-two where NW=4 RELIABLY
 * wins is 2^19 = 524288 MACs, so that is the shipped gate. Below it the choke
 * points stay serial (plan §2.4, §5). Tunable at runtime via env
 * PKERNEL_MATMUL_MIN_MACS (0 = always parallelize above PK_PARALLEL_MIN_ROWS;
 * a huge value = never). All the teacher ffn/head matmuls clear it; the d=128
 * student + R3 48x48 stay well below it.
 */
#ifndef PK_PARALLEL_MIN_MACS
#define PK_PARALLEL_MIN_MACS 524288
#endif

/*
 * Row-partition dispatch WITH the out*in size gate. `in_per_row` is the
 * contraction length (cols) so the gate can weigh `out*in` MACs. When
 * out*in < the effective threshold (PKERNEL_MATMUL_MIN_MACS or the compile
 * default), this calls body(ctx,0,out) INLINE — the exact serial path, zero
 * overhead, byte-identical. Above threshold it forwards to pk_parallel_rows.
 * This is the function the teacher matmul choke points call.
 */
void pk_parallel_rows_gated(size_t out, size_t in_per_row,
                            pk_row_body body, void *ctx);

/* Resolve the effective MACs gate (env override or the compile default).
 * Exposed for the MC-1 crossover cert so it can report the live threshold. */
size_t pk_parallel_min_macs(void);

/* Did the LAST pk_parallel_rows_gated call actually dispatch to the pool
 * (1) or run the serial inline body (0)? Cert hook so the equivalence /
 * "small student stays serial" assertions can observe the gate decision.
 * Returns -1 when the pool TU is absent (always-serial fallback). */
int pk_parallel_last_was_parallel(void);

/*
 * Run body(ctx, i0, i1) over a partition of [0, out) across <= NW workers,
 * then JOIN. Deterministic: the partition is a pure function of (out, NW);
 * completion order is irrelevant because the output ranges are disjoint.
 *
 * NW is sysconf(_SC_NPROCESSORS_ONLN) capped to PK_PARALLEL_MAX_WORKERS,
 * overridable for the cert via the env var PKERNEL_MATMUL_THREADS.
 *
 * Fallback to a single INLINE body(ctx, 0, out) call (the exact serial path,
 * zero overhead, byte-identical) when: NW<=1, no pool, or out below the gate.
 *
 * The dispatching thread is worker 0 and BLOCKS in the join. Idle workers
 * BLOCK on a condvar (futex sleep) — they NEVER busy-spin (respects the
 * wave-idle-yield contract; a spinning pool would burn a phone battery).
 */
void pk_parallel_rows(size_t out, pk_row_body body, void *ctx);

/* Force a worker count for the next dispatches (cert hook; <=0 = honor env /
 * autodetect). Mirrors PKERNEL_MATMUL_THREADS. Hosted pool only; no-op when
 * the pool TU is absent. */
void pk_parallel_set_threads(int nw);

/* Diagnostic: total times a worker woke to drain a job since process start.
 * Used by the idle cert to assert workers BLOCK (this counter only advances
 * on real dispatch, never on spin). Returns 0 when the pool TU is absent. */
unsigned long pk_parallel_wake_count(void);

#ifdef __cplusplus
}
#endif

#endif /* PKERNEL_PK_PARALLEL_H */
