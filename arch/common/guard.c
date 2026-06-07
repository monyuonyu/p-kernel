/*
 *  guard.c
 *  Task fault isolation + auto-respawn supervisor (wave 7).
 *
 *  See guard.h for the design contract. The flow for one fault:
 *
 *    task writes to NULL
 *      -> arch fault handler (sigaltstack / abort vector)
 *      -> guard_fault_isolate(): is the dying task in the guard
 *         table, and is kernel state still trustworthy? If yes,
 *         hand back the emergency stack.
 *      -> arch handler rewrites ONLY the named PC/SP registers of
 *         the interrupted context; sigreturn resumes the task at
 *         guard_task_killer on the emergency stack.
 *      -> guard_task_killer(): ordinary task context again. Log,
 *         mark DEAD, wake the supervisor, tk_exd_tsk(). The dying
 *         task is gone; every other task keeps running.
 *      -> guard supervisor task: after a backoff that doubles per
 *         death (cap GUARD_MAX_DEATHS), call recover_fn (dtr:
 *         reload weights from p-fs) and tk_cre_tsk/tk_sta_tsk a
 *         fresh incarnation.
 *
 *  arch/common discipline: no <string.h>, static buffers, output
 *  via sio_send_frame. Nothing here is persisted or sent on the
 *  wire, so the structs below are process-local only.
 */

#include "guard.h"
#include "kernel.h"
#include "task.h"          /* knl_ctxtsk / knl_dispatch_disabled */

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ------------------------------------------------------------------ */
/* output helpers                                                      */
/* ------------------------------------------------------------------ */

static void gd_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void gd_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { gd_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    gd_puts(&buf[i]);
}

/* full-width hex for fault PC/address (8 digits on ILP32 targets,
 * 16 on LP64 — sized from unsigned long, diagnostics only) */
static void gd_puthexl(unsigned long v)
{
    char buf[2 * sizeof(unsigned long) + 1];
    INT  n = (INT)(2 * sizeof(unsigned long));
    buf[n] = '\0';
    for (INT k = n - 1; k >= 0; k--) {
        INT d = (INT)(v & 0xF);
        buf[k] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    }
    gd_puts(buf);
}

/* ------------------------------------------------------------------ */
/* module state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    UB               state;          /* GUARD_ST_*                     */
    char             name[GUARD_NAME_LEN];
    FP               entry;
    W                stack_size;
    W                priority;
    GUARD_RECOVER_FN recover_fn;
    ID               tid;            /* current incarnation (when RUNNING) */
    UW               deaths;         /* fault count                    */
    UW               last_death_ms;  /* SYSTIM.lo at fault             */
    UW               last_recover_ms;/* SYSTIM.lo at last respawn      */
    /* last fault diagnostics (filled in signal context)               */
    W                f_sig;
    unsigned long    f_pc;
    unsigned long    f_addr;
} GUARD_ENT;

static GUARD_ENT g_tab[GUARD_MAX];
static ID        g_sup_tid  = 0;
static UB        g_inited   = 0;

/* one-kill-at-a-time latch. Set in signal context by
 * guard_fault_isolate, cleared by guard_task_killer once the dying
 * task is committed to exit. volatile: written from the fault
 * handler, read from task context. */
static volatile UB g_killing   = 0;
static volatile W  g_dying_idx = -1;

/* Emergency stack the killer runs on. The dying task's own stack may
 * be the very thing that broke (overflow), so tk_exd_tsk must not run
 * there. Only one kill is in flight at a time (g_killing), so a
 * single static stack suffices. */
#define GUARD_DEATH_STACK_SZ  8192
static UB g_death_stack[GUARD_DEATH_STACK_SZ] __attribute__((aligned(16)));

static UW now_ms(void)
{
    SYSTIM t;
    tk_get_tim(&t);
    return t.lo;
}

/* ------------------------------------------------------------------ */
/* fault-capture hooks                                                 */
/* ------------------------------------------------------------------ */

/* SIGNAL/ABORT CONTEXT. No kernel calls, no prints — just decide and
 * record. The arch handler has already rejected faults taken inside
 * an IRQ-disabled window (arch_irq_disabled_flag). */
void *guard_fault_isolate(int sig, unsigned long pc, unsigned long addr)
{
    if (g_killing) return NULL;            /* second fault mid-kill    */
    if (in_indp()) return NULL;            /* handler ctx / no task    */
    if (knl_dispatch_disabled) return NULL;/* dispatcher state frozen  */

    ID tid = knl_ctxtsk->tskid;
    for (INT i = 0; i < GUARD_MAX; i++) {
        GUARD_ENT *e = &g_tab[i];
        if (e->state != GUARD_ST_RUNNING || e->tid != tid) continue;
        e->f_sig  = (W)sig;
        e->f_pc   = pc;
        e->f_addr = addr;
        g_dying_idx = (W)i;
        g_killing   = 1;
        return (void *)&g_death_stack[GUARD_DEATH_STACK_SZ];
    }
    return NULL;                           /* unguarded task -> abort  */
}

/* TASK CONTEXT, emergency stack. The fault handler already returned;
 * the kernel believes this task is running normal code, so kernel
 * calls are legal again. Never returns. */
void guard_task_killer(void)
{
    GUARD_ENT *e = &g_tab[g_dying_idx];

    gd_puts("[guard] FAULT in task '"); gd_puts(e->name);
    gd_puts("' sig="); gd_putdec((UW)e->f_sig);
    gd_puts(" pc=0x");   gd_puthexl(e->f_pc);
    gd_puts(" addr=0x"); gd_puthexl(e->f_addr);
    gd_puts("\r\n[guard] isolating: killing task only — kernel and"
            " all other tasks keep running\r\n");

    e->deaths++;
    e->last_death_ms = now_ms();
    e->state = GUARD_ST_DEAD;
    e->tid   = 0;
    g_killing = 0;

    if (g_sup_tid > 0) tk_wup_tsk(g_sup_tid);   /* best effort */

    tk_exd_tsk();                               /* no return */
    for (;;) ;                                  /* paranoia */
}

/* ------------------------------------------------------------------ */
/* spawn / respawn                                                     */
/* ------------------------------------------------------------------ */

static ID guard_spawn(GUARD_ENT *e)
{
    T_CTSK ct;
    ct.exinf   = NULL;
    ct.tskatr  = TA_HLNG | TA_RNG0;
    ct.task    = e->entry;
    ct.itskpri = (PRI)e->priority;
    ct.stksz   = e->stack_size;
    ID id = tk_cre_tsk(&ct);
    if (id < E_OK) return id;
    ER er = tk_sta_tsk(id, 0);
    if (er < E_OK) { tk_del_tsk(id); return (ID)er; }
    return id;
}

/* ------------------------------------------------------------------ */
/* supervisor task                                                     */
/* ------------------------------------------------------------------ */

static void guard_sup_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    for (;;) {
        tk_slp_tsk(GUARD_POLL_MS);   /* woken early by the killer */

        for (INT i = 0; i < GUARD_MAX; i++) {
            GUARD_ENT *e = &g_tab[i];
            if (e->state != GUARD_ST_DEAD) continue;

            if (e->deaths > GUARD_MAX_DEATHS) {
                e->state = GUARD_ST_GIVEN_UP;
                gd_puts("[guard] '"); gd_puts(e->name);
                gd_puts("' exceeded "); gd_putdec(GUARD_MAX_DEATHS);
                gd_puts(" deaths — giving up (broken code, not bad luck)\r\n");
                continue;
            }

            /* exponential backoff: 200ms, 400ms, 800ms, ... */
            UW backoff = GUARD_BACKOFF_MS << (e->deaths - 1);
            if ((UW)(now_ms() - e->last_death_ms) < backoff)
                continue;                 /* not yet — poll again      */

            if (e->recover_fn) {
                gd_puts("[guard] running recover_fn for '");
                gd_puts(e->name); gd_puts("'\r\n");
                e->recover_fn();
            }

            ID id = guard_spawn(e);
            if (id < E_OK) {
                gd_puts("[guard] respawn of '"); gd_puts(e->name);
                gd_puts("' failed err="); gd_putdec((UW)(-id));
                gd_puts(" — will retry\r\n");
                e->last_death_ms = now_ms();    /* re-arm backoff      */
                continue;
            }
            e->tid             = id;
            e->state           = GUARD_ST_RUNNING;
            e->last_recover_ms = now_ms();
            gd_puts("[guard] respawned '"); gd_puts(e->name);
            gd_puts("' tid="); gd_putdec((UW)id);
            gd_puts(" (death "); gd_putdec(e->deaths);
            gd_puts("/"); gd_putdec(GUARD_MAX_DEATHS);
            gd_puts(")\r\n");
        }
    }
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

void guard_init(void)
{
    if (g_inited) return;
    for (INT i = 0; i < GUARD_MAX; i++) g_tab[i].state = GUARD_ST_FREE;

    T_CTSK ct;
    ct.exinf   = NULL;
    ct.tskatr  = TA_HLNG | TA_RNG0;
    ct.task    = (FP)guard_sup_task;
    ct.itskpri = 4;                  /* above the AI workers it guards */
    ct.stksz   = 4096;
    ID id = tk_cre_tsk(&ct);
    if (id < E_OK || tk_sta_tsk(id, 0) < E_OK) {
        gd_puts("[guard] supervisor task creation FAILED\r\n");
        return;
    }
    g_sup_tid = id;
    g_inited  = 1;
    gd_puts("[guard] task fault-isolation supervisor ready\r\n");
}

W guard_register(const char *name, FP entry, W stack_size, W priority,
                 GUARD_RECOVER_FN recover_fn)
{
    for (INT i = 0; i < GUARD_MAX; i++) {
        GUARD_ENT *e = &g_tab[i];
        if (e->state != GUARD_ST_FREE) continue;

        INT j = 0;
        while (name[j] && j < GUARD_NAME_LEN - 1) { e->name[j] = name[j]; j++; }
        e->name[j] = '\0';
        e->entry           = entry;
        e->stack_size      = stack_size;
        e->priority        = priority;
        e->recover_fn      = recover_fn;
        e->deaths          = 0;
        e->last_death_ms   = 0;
        e->last_recover_ms = 0;
        e->f_sig = 0; e->f_pc = 0; e->f_addr = 0;

        ID id = guard_spawn(e);
        if (id < E_OK) {
            e->state = GUARD_ST_FREE;
            gd_puts("[guard] spawn of '"); gd_puts(e->name);
            gd_puts("' failed err="); gd_putdec((UW)(-id)); gd_puts("\r\n");
            return (W)id;
        }
        e->tid   = id;
        e->state = GUARD_ST_RUNNING;
        gd_puts("[guard] guarding '"); gd_puts(e->name);
        gd_puts("' tid="); gd_putdec((UW)id);
        gd_puts(" pri="); gd_putdec((UW)priority);
        gd_puts(recover_fn ? " (with recover_fn)\r\n" : "\r\n");
        return (W)id;
    }
    gd_puts("[guard] table full\r\n");
    return E_LIMIT;
}

void guard_print(void)
{
    static const char *st_name[] = { "free", "RUNNING", "DEAD", "GIVEN-UP" };
    gd_puts("[guard] supervisor: ");
    gd_puts(g_inited ? "active" : "NOT INITIALIZED");
    gd_puts("\r\n  name             state     tid  deaths  last-fault"
            "        last-recover-ms\r\n");
    INT found = 0;
    for (INT i = 0; i < GUARD_MAX; i++) {
        GUARD_ENT *e = &g_tab[i];
        if (e->state == GUARD_ST_FREE) continue;
        found++;
        gd_puts("  "); gd_puts(e->name);
        { INT len = 0; while (e->name[len]) len++;
          for (; len < GUARD_NAME_LEN; len++) gd_puts(" "); }
        gd_puts(st_name[e->state]); gd_puts("  ");
        gd_putdec((UW)e->tid);      gd_puts("    ");
        gd_putdec(e->deaths);       gd_puts("      ");
        if (e->deaths) {
            gd_puts("sig="); gd_putdec((UW)e->f_sig);
            gd_puts("@0x");  gd_puthexl(e->f_pc);
        } else {
            gd_puts("-");
        }
        gd_puts("  ");
        gd_putdec(e->last_recover_ms);
        gd_puts("\r\n");
    }
    if (!found) gd_puts("  (no guarded tasks)\r\n");
}
