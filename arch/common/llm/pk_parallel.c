/*
 *  pk_parallel.c — HOSTED (Linux/Android) pthread worker pool for the
 *  deterministic row-partitioned matmul dispatch (MC-0).
 *
 *  See pk_parallel.h for the invariant and docs/architecture/
 *  multicore-matmul-plan.md §3.2 (Phase A), §5 (idle discipline).
 *
 *  This is a MATH-ONLY pool. It NEVER touches knl_ctxtsk / knl_schedtsk and
 *  never runs a T-Kernel task — the kernel scheduler stays single-threaded.
 *  It exists only to drain a partition of matmul output rows and then sleep.
 *
 *  Build: -O1 -ffp-contract=off (one mind, one math). Link: -lpthread.
 */
#include "pk_parallel.h"

#define PK_PARALLEL_HAVE_POOL 1

#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#define PK_PARALLEL_MAX_WORKERS 8

/* One dispatch's shared state. The dispatching thread fills body/ctx/out/nw,
 * publishes a new generation, wakes the helpers, then runs slot 0 itself and
 * joins on `done`. Helpers block on `gen` advancing — they never spin. */
struct pk_pool {
    pthread_mutex_t  mtx;
    pthread_cond_t   job_cv;     /* helpers sleep here until a new gen      */
    pthread_cond_t   done_cv;    /* dispatcher sleeps here until all done   */

    int              nhelpers;   /* worker threads spun up = NW-1           */
    pthread_t        tid[PK_PARALLEL_MAX_WORKERS - 1];

    /* current job */
    unsigned long    gen;        /* bumped once per dispatch                */
    pk_row_body      body;
    void            *ctx;
    size_t           out;
    int              nw;         /* slices this dispatch was partitioned in  */
    int              done;       /* helper slots that have finished          */

    int              shutdown;   /* process teardown (best-effort)           */
    unsigned long    wakes;      /* diagnostic: helper drains since start    */
};

static struct pk_pool g_pool;
static pthread_once_t  g_once   = PTHREAD_ONCE_INIT;
static int             g_inited = 0;       /* set iff pool came up cleanly  */
static int             g_force_nw = 0;     /* cert override; <=0 = auto      */

/* ---- the deterministic partition (pure function of (out, NW)) ---------- */
/* Slice s in [0,nw) owns rows [s*q + min(s,r), ...). A ragged remainder
 * (out % nw) is given to the LAST slice — matches the plan §1, §8 risk 5. */
static void pk_slice(size_t out, int nw, int s, size_t *i0, size_t *i1)
{
    size_t q = out / (size_t)nw;
    /* first (nw-1) slices get q rows each; the last gets the remainder too. */
    *i0 = (size_t)s * q;
    *i1 = (s == nw - 1) ? out : (*i0 + q);
}

/* ---- helper thread main loop (BLOCKS when idle) ------------------------ */
static void *pk_worker_main(void *arg)
{
    /* slot index is 1..nhelpers; slot 0 is run by the dispatcher itself. */
    int slot = (int)(long)arg;
    unsigned long seen = 0;

    pthread_mutex_lock(&g_pool.mtx);
    for (;;) {
        /* block (futex sleep) until a new generation is published or
         * shutdown. NO busy-spin: this is the wave-idle-yield contract. */
        while (g_pool.gen == seen && !g_pool.shutdown)
            pthread_cond_wait(&g_pool.job_cv, &g_pool.mtx);
        if (g_pool.shutdown) break;
        seen = g_pool.gen;

        /* snapshot this job's parameters */
        pk_row_body body = g_pool.body;
        void       *ctx  = g_pool.ctx;
        size_t      out  = g_pool.out;
        int         nw   = g_pool.nw;
        g_pool.wakes++;
        pthread_mutex_unlock(&g_pool.mtx);

        /* this helper only does WORK if the dispatch used >slot slices, but
         * EVERY helper reports done so the join counter is unambiguous (it
         * waits for all nhelpers, independent of nw). */
        if (slot < nw) {
            size_t i0, i1;
            pk_slice(out, nw, slot, &i0, &i1);
            if (i1 > i0) body(ctx, i0, i1);
        }

        pthread_mutex_lock(&g_pool.mtx);
        if (++g_pool.done >= g_pool.nhelpers)
            pthread_cond_signal(&g_pool.done_cv);
    }
    pthread_mutex_unlock(&g_pool.mtx);
    return NULL;
}

static void pk_pool_init(void)
{
    memset(&g_pool, 0, sizeof(g_pool));

    int nproc = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc < 1) nproc = 1;
    int nw = nproc;
    if (nw > PK_PARALLEL_MAX_WORKERS) nw = PK_PARALLEL_MAX_WORKERS;
    /* nhelpers = nw-1; the dispatcher is worker 0. Capacity is the worker
     * MAX, but we spin up only as many helpers as the host has cores so an
     * idle box keeps zero extra threads. */
    g_pool.nhelpers = nw - 1;

    if (pthread_mutex_init(&g_pool.mtx, NULL) != 0)   return;
    if (pthread_cond_init(&g_pool.job_cv, NULL) != 0) return;
    if (pthread_cond_init(&g_pool.done_cv, NULL) != 0) return;

    int up = 0;
    for (int s = 1; s <= g_pool.nhelpers; s++) {
        if (pthread_create(&g_pool.tid[s - 1], NULL,
                           pk_worker_main, (void *)(long)s) != 0)
            break;
        up++;
    }
    g_pool.nhelpers = up;     /* however many actually started */
    g_inited = 1;             /* even with 0 helpers: inline fast path works */
}

/* ---- public API -------------------------------------------------------- */
void pk_parallel_set_threads(int nw)
{
    g_force_nw = nw;
}

unsigned long pk_parallel_wake_count(void)
{
    return g_inited ? g_pool.wakes : 0UL;
}

/* Resolve the effective worker count for THIS dispatch:
 *   cert override (set_threads) > env PKERNEL_MATMUL_THREADS > nhelpers+1,
 * capped to MAX and to (nhelpers+1) so we never ask for threads we lack. */
static int pk_effective_nw(void)
{
    int nw = 0;
    if (g_force_nw > 0) {
        nw = g_force_nw;
    } else {
        const char *e = getenv("PKERNEL_MATMUL_THREADS");
        if (e && *e) nw = atoi(e);
    }
    if (nw <= 0) nw = g_pool.nhelpers + 1;        /* autodetect */
    if (nw > g_pool.nhelpers + 1) nw = g_pool.nhelpers + 1;
    if (nw > PK_PARALLEL_MAX_WORKERS) nw = PK_PARALLEL_MAX_WORKERS;
    if (nw < 1) nw = 1;
    return nw;
}

void pk_parallel_rows(size_t out, pk_row_body body, void *ctx)
{
    pthread_once(&g_once, pk_pool_init);

    /* Fallback: serial INLINE body (byte-identical, zero overhead) when the
     * pool failed to come up, the row count is below the gate, or only one
     * worker would run anyway. */
    if (!g_inited || out < PK_PARALLEL_MIN_ROWS) {
        body(ctx, 0, out);
        return;
    }

    int nw = pk_effective_nw();
    if (nw <= 1) {
        body(ctx, 0, out);
        return;
    }

    pthread_mutex_lock(&g_pool.mtx);
    g_pool.body = body;
    g_pool.ctx  = ctx;
    g_pool.out  = out;
    g_pool.nw   = nw;
    g_pool.done = 0;
    g_pool.gen++;                       /* publish the job */
    pthread_cond_broadcast(&g_pool.job_cv);
    pthread_mutex_unlock(&g_pool.mtx);

    /* the dispatcher runs slice 0 itself (worker 0) while helpers run 1..nw-1 */
    {
        size_t i0, i1;
        pk_slice(out, nw, 0, &i0, &i1);
        if (i1 > i0) body(ctx, i0, i1);
    }

    /* JOIN: block (no spin) until ALL helpers have reported for this gen.
     * Helpers whose slot >= nw did no work but still report, so the counter
     * is unambiguous regardless of how many slices this dispatch used. */
    pthread_mutex_lock(&g_pool.mtx);
    while (g_pool.done < g_pool.nhelpers)
        pthread_cond_wait(&g_pool.done_cv, &g_pool.mtx);
    pthread_mutex_unlock(&g_pool.mtx);
}
