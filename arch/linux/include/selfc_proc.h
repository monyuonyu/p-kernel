/*
 *  arch/linux/include/selfc_proc.h — the germ-process crash boundary for
 *  self-built units (docs/architecture/50-evolution/selfc-ring3.md §2.1, CDN-S1(a)).
 *
 *  selfc compiles a unit OF the node, in the node's own address space
 *  (selfc.c, reused byte-for-byte up to entry resolution). This module
 *  takes the resolved entry and runs it where a crash is SURVIVABLE: a
 *  fork()ed germ process. A wild write in the unit hits the child's COW
 *  copy, never the parent; the child's SIGSEGV is an EVENT the parent
 *  reaps with waitpid(), not a fault that kills the node.
 *
 *  Hosted-only (fork/waitpid/socketpair are port-layer; this file never
 *  compiles on bare metal). Single-threadedness of arch/linux is what
 *  makes fork() safe here (no fork+threads lock hazards) — a fact to
 *  PRESERVE (§6 FLAGGED): a future pthreads wave invalidates it.
 *
 *  The 6-symbol selfc API table (selfc.c) becomes a SYSCALL boundary: in
 *  the germ child the proxy stubs marshal LOG/DELAY/OPEN/PUB/SUB over a
 *  socketpair to the parent, which validates every frame as hostile input
 *  (length-checked, op-whitelisted, topics confined to unit/<name>/). This
 *  is the hosted mirror of ring3-core's int 0x80 (§1.2).
 */
#pragma once
#include "kernel.h"

#ifdef HAVE_LIBTCC

/* ---- the unit-frame wire (child <-> parent, AF_UNIX SOCK_SEQPACKET) --- */
/* fixed-size 8-byte header + <=512-byte payload, fixed-width LP64 wire     */
/* discipline (world.h). The parent treats every frame as hostile input.   */

#define SELFC_SYS_LOG    1     /* tm_printf  -> parent prints [unit:<name>] */
#define SELFC_SYS_OPEN   2     /* kdds_open  (topic confined to unit/<name>)*/
#define SELFC_SYS_PUB    3     /* kdds_pub                                   */
#define SELFC_SYS_SUB    4     /* kdds_sub                                   */
/* tk_dly_tsk is child-local (clock_nanosleep) — NO frame.                  */
/* tk_slp_tsk is REMOVED from the v1 table (no cross-process wakeup, §1.2). */

#define SELFC_FRAME_PAYLOAD_MAX  512

typedef struct {
    U1  op;        /* SELFC_SYS_*                                  */
    U1  flags;     /* op-specific (e.g. kdds qos for OPEN)         */
    U2  len;       /* payload byte count (<= SELFC_FRAME_PAYLOAD_MAX) */
    U4  arg;       /* op-specific (e.g. handle for PUB/SUB)        */
} __attribute__((packed)) SELFC_FRAME_HDR;   /* 8 bytes */

_Static_assert(sizeof(SELFC_FRAME_HDR) == 8, "SELFC_FRAME_HDR wire must be 8B");

/* ---- the capability proxy stubs (bound into the API table when         */
/*      isolation is on; they marshal over the socketpair in the child).   */
/*      Never called in the parent — the parent only runs the supervisor   */
/*      and the frame dispatcher.                                          */
int  selfc_proxy_printf(const unsigned char *fmt, ...);
int  selfc_proxy_dly_tsk(unsigned int ms);
int  selfc_proxy_kdds_open(const char *name, int qos);
int  selfc_proxy_kdds_pub(int h, const void *data, int len);
int  selfc_proxy_kdds_sub(int h, void *buf, int buflen, int timeout_ms);

/* ---- germ lifecycle (the supervisor table owns it) ------------------- */

/* Request germination of unit <name>@<unit_seq>. The request is ENQUEUED; the
 * SUPERVISOR task compiles (selfc_resolve_unit) and fork()s it — ALL tcc
 * compilation and forking happen in the one supervisor task, never the shell
 * task (forking from the shell corrupts its stdin; concurrent tcc from two
 * tasks corrupts the shared compiler state). Returns 0 if accepted. */
int  selfc_germ_launch(const char *name, U4 unit_seq);

/* The supervisor T-Kernel task: waitpid(WNOHANG) on its cadence, drain the
 * unit's log/pub frames, and apply the §1.4 rollback rule on death. One
 * task owns all germ lifecycle (anti-fork §6). */
void selfc_proc_task(INT stacd, void *exinf);

/* one supervisor pass (drain frames + reap exited germs) — the SAME body the
 * resident task runs, exposed so `selfc test` can pump it inline when net
 * (and thus the task) is not up. */
void selfc_proc_pump(void);

/* observability counters for the cert (exact comparisons, §5) */
extern volatile U4 selfc_reaped_count;    /* total germ reaps this boot   */
extern volatile int selfc_last_termsig;   /* WTERMSIG of the last reap    */
extern volatile int selfc_last_signaled;  /* WIFSIGNALED of the last reap */

/* cert hooks (selfc test): per-name running seq, BAD marks, deaths.       */
int  selfc_sup_running_seq(const char *name);  /* current seq, -1 if none  */
int  selfc_sup_seq_is_bad(const char *name, U4 seq);
int  selfc_sup_deaths(const char *name);
int  selfc_sup_is_dead(const char *name);      /* unit DEAD (seq1 BAD)      */
void selfc_sup_reset(void);                /* clear table (cert isolation)  */
void selfc_sup_set_probation(UW ms);       /* cert: shorten probation window */

/* set by the germ child just before it calls selfc_main(); the proxy stubs
 * check it (in the parent it is 0 — the stubs would be a bug there). */
extern volatile int selfc_in_child;

#endif /* HAVE_LIBTCC */
