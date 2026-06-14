/*
 *  tcb_churn.c — KILL-CHURN-CRASH portable reproducer harness
 *  ----------------------------------------------------------
 *  gap-ledger row KILL-CHURN-CRASH (OPEN).
 *
 *  HISTORY (see docs/architecture/gap-ledger.md): the historic crash was
 *  an x86-specific ring0 #PF with a garbage PC during kill/heal churn of
 *  ring3 ELF daemons.  The residual was localized to a SUSPECTED portable
 *  use-after-free of a RECYCLED task's TCB whose embedded wait-timer
 *  (TMEB `wtmeb`) is still a live node in the kernel-wide timer queue when
 *  it fires, after the TCB was freed by tk_del_tsk and recycled by a new
 *  tk_cre_tsk.  The x86 `dproc churn` verb (arch/x86/shell.c) drives that
 *  via real ELF daemons and NO LONGER reproduces on master (24/24 PASS).
 *
 *  This harness strips the mechanism down to PORTABLE T-Kernel primitives
 *  (no ELF, no ring3, no vfs) so it runs on EVERY target including
 *  boot/linux.  It exercises EXACTLY the named race:
 *
 *    1. a victim task blocks in a timed wait (tk_dly_tsk / tk_wai_sem with
 *       a finite timeout) -> knl_make_wait_reltim arms tcb->wtmeb as a live
 *       node in the kernel timer queue with callback knl_wait_release_tmout.
 *    2. a killer foreign-terminates + deletes it (tk_ter_tsk + tk_del_tsk),
 *       the same non-atomic sequence dproc_kill_by_name uses, at a VARIED
 *       phase so the kill lands at every point of the victim's wait/dispatch
 *       cycle (incl. the instant the timer is about to fire).
 *    3. the freed TCB is IMMEDIATELY recycled by a fresh tk_cre_tsk (the
 *       FreeQue is LIFO -> QueInsert at head -> the just-freed TCB is the
 *       next one handed out), reproducing the recycle the heal re-exec did.
 *    4. a heartbeat sentinel proves the scheduler keeps advancing; a wedge
 *       or a garbage-PC fault stops it (or crashes the process outright).
 *
 *  PASS = survived all cycles, sentinel advanced, no crash.
 *  A crash / SIGSEGV / wedge here is the disease.  Build the linux target
 *  with ASAN (make ASAN=1) to turn a latent UAF into a loud diagnostic.
 */

#include "kernel.h"
#include <tmonitor.h>

/* ---- tiny output (no libc printf dependency in the kernel) ---------- */
static void cz_puts(const char *s) { tm_putstring((UB *)s); }
static void cz_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { cz_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    cz_puts(&buf[i]);
}

/* ---- heartbeat sentinel: a live-scheduler witness ------------------- */
static volatile UW cz_sentinel_ticks;
static volatile W  cz_sentinel_stop;

static void cz_sentinel(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    while (!cz_sentinel_stop) {
        cz_sentinel_ticks++;
        tk_dly_tsk(5);
    }
    tk_ext_tsk();
}

/* ---- victim: blocks in a timed wait so its wtmeb is armed ----------- */
/* The victim arms a finite-timeout wait (knl_make_wait_reltim) and loops.
 * It is designed to be terminated from OUTSIDE at an arbitrary phase. */
static void cz_victim(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    /* A finite timeout arms tcb->wtmeb in the global timer queue.
     * The loop re-arms continuously so the killer can land mid-wait. */
    for (;;) {
        tk_dly_tsk(7);   /* -> knl_make_wait_reltim, wtmeb live in timer q */
    }
    /* never reached; killed externally */
}

#ifndef TCBCHURN_CYCLES
#define TCBCHURN_CYCLES 400
#endif

void tcb_churn(W cycles, W concurrency);
void tcb_churn_pri(W cycles, W concurrency, W vpri);

/*
 * tcb_churn — drive the kill-during-recycle race deterministically.
 *
 * `cycles`      number of spawn->kill->recycle rounds (0 = default).
 * `concurrency` victims per round (1..8) — concurrent kills widen the race.
 * `vpri`        victim priority (0 = default 8). Pass a HIGH priority (e.g.
 *               1, above the churn driver's shell-task priority) so a victim
 *               can be the top runnable task — i.e. knl_schedtsk — at the
 *               instant tk_del_tsk runs, directly probing the wave-40
 *               schedtsk-guard window the ledger names.
 */
void tcb_churn(W cycles, W concurrency) { tcb_churn_pri(cycles, concurrency, 0); }

void tcb_churn_pri(W cycles, W concurrency, W vpri)
{
    if (cycles      <= 0) cycles = TCBCHURN_CYCLES;
    if (concurrency <= 0) concurrency = 1;
    if (concurrency >  8) concurrency = 8;
    if (vpri <= 0) vpri = 8;

    cz_sentinel_ticks = 0;
    cz_sentinel_stop  = 0;

    T_CTSK cs = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                  .task = cz_sentinel, .itskpri = 9, .stksz = 4096 };
    ID sent = tk_cre_tsk(&cs);
    if (sent < E_OK || tk_sta_tsk(sent, 0) < E_OK) {
        cz_puts("tcb-churn: FAIL sentinel-create\r\n");
        cz_puts("[tcb-churn] FAIL\r\n");
        return;
    }

    cz_puts("tcb-churn: START cycles="); cz_putdec((UW)cycles);
    cz_puts(" concurrency="); cz_putdec((UW)concurrency);
    cz_puts("\r\n");

    UW  base_ticks = cz_sentinel_ticks;
    W   created = 0, killed = 0;

    T_CTSK vc = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                  .task = cz_victim, .itskpri = (PRI)vpri, .stksz = 4096 };

    for (W c = 0; c < cycles; c++) {
        ID v[8];
        /* (1) spawn `concurrency` victims, each blocking in a timed wait */
        for (W j = 0; j < concurrency; j++) {
            v[j] = tk_cre_tsk(&vc);
            if (v[j] >= E_OK) {
                tk_sta_tsk(v[j], 0);
                created++;
            }
        }
        /* (2) let them reach the timed wait and arm their wtmeb, then kill
         *     at a VARIED phase 0..7 so the kill lands at every point of
         *     the wait/dispatch/timer cycle (incl. just before fire). */
        tk_dly_tsk((RELTIM)(c & 7));

        for (W j = 0; j < concurrency; j++) {
            if (v[j] < E_OK) continue;
            /* exact dproc_kill_by_name sequence, NOT one critical section */
            tk_ter_tsk(v[j]);
            tk_del_tsk(v[j]);
            killed++;
        }
        /* (3) IMMEDIATELY recycle: the next round's tk_cre_tsk gets the
         *     just-freed TCB off the LIFO FreeQue, reproducing the recycle
         *     the heal re-exec did concurrently with a pending timer fire.
         * (4) tiny settle so a pending timer fire can race the recycle */
        if ((c & 3) == 0) tk_dly_tsk(1);

        if ((c & 63) == 63) {
            cz_puts("tcb-churn: cycle "); cz_putdec((UW)(c + 1));
            cz_puts("  created="); cz_putdec((UW)created);
            cz_puts(" killed=");   cz_putdec((UW)killed);
            cz_puts(" ticks=");    cz_putdec(cz_sentinel_ticks);
            cz_puts("\r\n");
        }
    }

    tk_dly_tsk(50);
    UW end_ticks = cz_sentinel_ticks;
    cz_sentinel_stop = 1;
    tk_dly_tsk(20);
    if (sent >= E_OK) tk_del_tsk(sent);

    BOOL sched_alive = (end_ticks > base_ticks);

    cz_puts("tcb-churn: cycles=");  cz_putdec((UW)cycles);
    cz_puts("  created=");          cz_putdec((UW)created);
    cz_puts("  killed=");           cz_putdec((UW)killed);
    cz_puts("  sched_ticks ");      cz_putdec(base_ticks);
    cz_puts("->");                  cz_putdec(end_ticks);
    cz_puts("\r\n");

    if (sched_alive && created > 0 && killed > 0) {
        cz_puts("tcb-churn: PASS  (no UAF crash, sched alive)\r\n");
        cz_puts("[tcb-churn] PASS\r\n");
    } else {
        cz_puts("tcb-churn: FAIL  (sched_alive=");
        cz_putdec((UW)sched_alive);
        cz_puts(" created="); cz_putdec((UW)created);
        cz_puts(" killed=");  cz_putdec((UW)killed);
        cz_puts(")\r\n");
        cz_puts("[tcb-churn] FAIL\r\n");
    }
}
