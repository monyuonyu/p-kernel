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
 *  When the pool TU is not linked (PK_PARALLEL_HAVE_POOL undefined), the
 *  inline fallback below runs body() serially — identical bits, zero deps.
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
