/*
 *  tk_conform.c — the contract suite, version 2 (inbox #5).
 *
 *  p-kernel's sanctuary is not the kernel's insides but the promises it makes
 *  to user space: the μT-Kernel 3.0 tk_* service calls the shell / mind /
 *  distributed tasks actually use. This file checks those promises from the
 *  outside — normal paths, error codes and boundaries — so that the kernel may
 *  be changed (or one day swapped) as long as this stays green.
 *
 *  Scope: tasks (create/start/exit/delete/priority/sleep/wake), semaphores
 *  (create/signal/wait with TMO_POL, a timeout, TMO_FEVR, delete), event flags
 *  (AND/OR waits, TWF_BITCLR, TA_WSGL, delete), mutexes (E_ILUSE, TA_INHERIT
 *  priority inheritance), and since v2 message buffers (copy, FIFO, size
 *  limits, full buffer), mailboxes (TA_MPRI order, the packet address is
 *  passed, not copied), fixed-size memory pools (hand-off to a waiter) and
 *  cyclic handlers (start / period / stop / start phase); time (tk_dly_tsk,
 *  tk_get_otm); dispatch disable (tk_dis_dsp / tk_ena_dsp / tk_ref_sys).
 *  Error codes: E_PAR, E_ID, E_NOEXS, E_OBJ, E_QOVR, E_TMOUT, E_DLT, E_ILUSE,
 *  E_CTX.
 *  v1's expectations were checked against the μT-Kernel 3.0 specification
 *  text (TRON Forum, mtk3_spec_jp) by audit-15; v2's were written from that
 *  text. Checks whose expectation is the reference kernel's own reading, not
 *  the spec's, are marked [impl] (check_impl below): S6 and C7 (audit-17
 *  moved the mark from C8, which the spec does cover, to C7).
 *
 *  Output: one "[tkc] PASS|FAIL <id> <what>" line per check, then
 *  "[tkc] <p> PASS / <f> FAIL". Driven by tests/host/run_tk_conform.sh.
 *  The checks run in a task of their own at priority 20 (helpers at 19/21).
 */
#include "kernel.h"
#include "tk_conform.h"

static void (*g_out)(const char *);
static INT g_pass, g_fail;

static void put_dec(char *b, INT *k, W v)
{
    char t[12]; INT n = 0;
    if (v < 0) { b[(*k)++] = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n) b[(*k)++] = t[--n];
}

/* "[tkc] PASS S3 what (got N)" */
static void check(BOOL ok, const char *id, const char *what, W got)
{
    char b[160]; INT k = 0; const char *p;
    for (p = ok ? "[tkc] PASS " : "[tkc] FAIL "; *p; ) b[k++] = *p++;
    for (p = id; *p && k < 40; ) b[k++] = *p++;
    b[k++] = ' ';
    for (p = what; *p && k < 140; ) b[k++] = *p++;
    for (p = " (got "; *p; ) b[k++] = *p++;
    put_dec(b, &k, got);
    b[k++] = ')'; b[k++] = '\r'; b[k++] = '\n'; b[k] = 0;
    g_out(b);
    if (ok) g_pass++; else g_fail++;
}

/* A check whose expectation comes from the reference kernel, not from the
 * μT-Kernel 3.0 specification text (audit-15 found S6 is one: the spec's
 * E_PAR for tk_wai_sem is only "tmout <= -2, cnt <= 0"). The id is printed
 * with "[impl]" in the text. A kernel that meets the spec may differ here, so
 * -DTKC_SPEC_ONLY drops these checks (the count then falls by their number;
 * that build is for trying another kernel behind the same promises, inbox #6). */
#ifdef TKC_SPEC_ONLY
#define check_impl(ok, id, what, got) ((void)0)
#else
#define check_impl(ok, id, what, got) check((ok), (id), (what), (got))
#endif

static W now_ms(void)
{
    SYSTIM t; tk_get_otm(&t);
    return (W)t.lo;
}

/* ---- helper tasks ---------------------------------------------------- */
static volatile INT h_flag;
static volatile ER  h_er1, h_er2;
static ID h_sem;

static void t_set_flag_and_exit(INT stacd, void *exinf)
{
    (void)exinf;
    h_flag = stacd;
    tk_ext_tsk();
}

static void t_sleep_then_exit(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    h_er1 = tk_slp_tsk(TMO_FEVR);
    h_flag = 1;
    tk_ext_tsk();
}

static void t_wait_sem_twice(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    h_er1 = tk_wai_sem(h_sem, 1, TMO_FEVR);
    h_flag = 1;
    h_er2 = tk_wai_sem(h_sem, 1, TMO_FEVR);   /* released by tk_del_sem */
    h_flag = 2;
    tk_ext_tsk();
}

static ID mk_task(void (*fn)(INT, void *), PRI pri)
{
    T_CTSK ct = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                  .task = (FP)fn, .itskpri = pri, .stksz = 8192 };
    return tk_cre_tsk(&ct);
}

static ID mk_sem(INT isem, INT maxsem)
{
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO,
                  .isemcnt = isem, .maxsem = maxsem };
    return tk_cre_sem(&cs);
}

/* ---- tasks ------------------------------------------------------------ */
static void suite_task(PRI me)
{
    T_RTSK r; ER er; ID t;
    T_CTSK bad = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                   .task = (FP)t_set_flag_and_exit, .itskpri = 0, .stksz = 8192 };

    /* task_manage.c tk_cre_tsk: @retval E_PAR (CHECK_PRI) */
    check(tk_cre_tsk(&bad) == E_PAR, "T1", "tk_cre_tsk itskpri=0 -> E_PAR", tk_cre_tsk(&bad));

    /* created task is DORMANT until started */
    t = mk_task(t_set_flag_and_exit, me - 1);
    check(t > 0, "T2", "tk_cre_tsk -> ID > 0", t);
    er = tk_ref_tsk(t, &r);
    check(er == E_OK && r.tskstat == TTS_DMT, "T3", "new task is TTS_DMT", r.tskstat);

    /* a HIGHER-priority task started by us runs before tk_sta_tsk returns */
    h_flag = 0;
    er = tk_sta_tsk(t, 7);
    check(er == E_OK && h_flag == 7, "T4", "higher-pri task runs before tk_sta_tsk returns (stacd=7)", h_flag);
    er = tk_ref_tsk(t, &r);
    check(er == E_OK && r.tskstat == TTS_DMT, "T5", "tk_ext_tsk -> back to TTS_DMT", r.tskstat);
    check(tk_del_tsk(t) == E_OK, "T6", "tk_del_tsk dormant -> E_OK", 0);
    check(tk_ref_tsk(t, &r) == E_NOEXS, "T7", "tk_ref_tsk deleted -> E_NOEXS", tk_ref_tsk(t, &r));
    check(tk_sta_tsk(t, 0) == E_NOEXS, "T8", "tk_sta_tsk deleted -> E_NOEXS", tk_sta_tsk(t, 0));

    /* a LOWER-priority task does not run until we block */
    t = mk_task(t_set_flag_and_exit, me + 1);
    h_flag = 0;
    tk_sta_tsk(t, 9);
    check(h_flag == 0, "T9", "lower-pri task has not run after tk_sta_tsk", h_flag);
    tk_dly_tsk(20);
    check(h_flag == 9, "T10", "lower-pri task ran once we slept", h_flag);
    tk_del_tsk(t);

    /* sleep / wake-up, and E_OBJ on a non-dormant task */
    t = mk_task(t_sleep_then_exit, me - 1);
    h_flag = 0; h_er1 = -999;
    tk_sta_tsk(t, 0);
    er = tk_ref_tsk(t, &r);
    check(er == E_OK && (r.tskstat & TTS_WAI) != 0 && h_flag == 0, "T11", "tk_slp_tsk(TMO_FEVR) -> TTS_WAI", r.tskstat);
    er = tk_sta_tsk(t, 0);
    check(er == E_OBJ, "T12", "tk_sta_tsk non-dormant -> E_OBJ", er);
    er = tk_del_tsk(t);
    check(er == E_OBJ, "T13", "tk_del_tsk non-dormant -> E_OBJ", er);
    er = tk_wup_tsk(t);
    check(er == E_OK && h_flag == 1 && h_er1 == E_OK, "T14", "tk_wup_tsk wakes it (tk_slp_tsk -> E_OK)", h_er1);
    tk_del_tsk(t);

    /* task_sync.c tk_slp_tsk: TMO_POL with no queued wake-up -> E_TMOUT */
    er = tk_slp_tsk(TMO_POL);
    check(er == E_TMOUT, "T15", "tk_slp_tsk(TMO_POL) nothing queued -> E_TMOUT", er);

    /* priority change (task_manage.c tk_chg_pri, CHECK_PRI_INI): an out-of-
     * range priority is E_PAR, but 0 is TPRI_INI = "back to the initial
     * priority" (v1's first draft expected E_PAR for 0 — that was wrong). */
    er = tk_chg_pri(TSK_SELF, 1000);
    check(er == E_PAR, "T16", "tk_chg_pri pri=1000 -> E_PAR", er);
    er = tk_chg_pri(TSK_SELF, me + 1);
    tk_ref_tsk(TSK_SELF, &r);
    check(er == E_OK && r.tskpri == me + 1, "T17", "tk_chg_pri(self, me+1) seen by tk_ref_tsk", r.tskpri);
    er = tk_chg_pri(TSK_SELF, TPRI_INI);
    tk_ref_tsk(TSK_SELF, &r);
    check(er == E_OK && r.tskpri == me, "T18", "tk_chg_pri(self, TPRI_INI) -> initial priority", r.tskpri);

    /* E_ID on an out-of-range id */
    check(tk_ref_tsk(-5, &r) == E_ID, "T19", "tk_ref_tsk id=-5 -> E_ID", tk_ref_tsk(-5, &r));
}

/* ---- semaphores --------------------------------------------------------- */
static void suite_sem(PRI me)
{
    ER er; ID s, t; W t0, dt;
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 2, .maxsem = 1 };

    /* semaphore.c tk_cre_sem: @retval E_PAR (maxsem >= isemcnt) */
    er = tk_cre_sem(&cs);
    check(er == E_PAR, "S1", "tk_cre_sem isemcnt>maxsem -> E_PAR", er);
    s = mk_sem(1, 1);
    check(s > 0, "S2", "tk_cre_sem(1,1) -> ID > 0", s);

    er = tk_wai_sem(s, 1, TMO_POL);
    check(er == E_OK, "S3", "tk_wai_sem(TMO_POL) with count 1 -> E_OK", er);
    er = tk_wai_sem(s, 1, TMO_POL);
    check(er == E_TMOUT, "S4", "tk_wai_sem(TMO_POL) with count 0 -> E_TMOUT", er);
    er = tk_wai_sem(s, 0, TMO_POL);
    check(er == E_PAR, "S5", "tk_wai_sem cnt=0 -> E_PAR", er);
    er = tk_wai_sem(s, 2, TMO_POL);
    check_impl(er == E_PAR, "S6", "[impl] tk_wai_sem cnt>maxsem -> E_PAR", er);
    er = tk_wai_sem(s, 1, -2);
    check(er == E_PAR, "S7", "tk_wai_sem tmout=-2 -> E_PAR", er);

    er = tk_sig_sem(s, 1);
    check(er == E_OK, "S8", "tk_sig_sem -> E_OK", er);
    er = tk_sig_sem(s, 1);
    check(er == E_QOVR, "S9", "tk_sig_sem over maxsem -> E_QOVR", er);
    er = tk_sig_sem(s, 0);
    check(er == E_PAR, "S10", "tk_sig_sem cnt=0 -> E_PAR", er);
    tk_wai_sem(s, 1, TMO_POL);               /* back to 0 */

    /* a real timeout: 50 ms, not earlier */
    t0 = now_ms();
    er = tk_wai_sem(s, 1, 50);
    dt = now_ms() - t0;
    check(er == E_TMOUT, "S11", "tk_wai_sem(50ms) on empty -> E_TMOUT", er);
    check(dt >= 50 && dt < 1000, "S12", "the 50 ms timeout lasted >= 50 ms", dt);

    /* a higher-priority waiter is released by tk_sig_sem at once, and by
     * tk_del_sem with E_DLT */
    h_sem = s; h_flag = 0; h_er1 = h_er2 = -999;
    t = mk_task(t_wait_sem_twice, me - 1);
    tk_sta_tsk(t, 0);
    check(h_flag == 0, "S13", "higher-pri waiter blocks on count 0", h_flag);
    er = tk_sig_sem(s, 1);
    check(er == E_OK && h_flag == 1 && h_er1 == E_OK, "S14", "tk_sig_sem releases it before returning", h_flag);
    er = tk_del_sem(s);
    check(er == E_OK && h_flag == 2 && h_er2 == E_DLT, "S15", "tk_del_sem releases the waiter with E_DLT", h_er2);
    tk_del_tsk(t);

    er = tk_wai_sem(s, 1, TMO_POL);
    check(er == E_NOEXS, "S16", "tk_wai_sem deleted -> E_NOEXS", er);
    er = tk_sig_sem(s, 1);
    check(er == E_NOEXS, "S17", "tk_sig_sem deleted -> E_NOEXS", er);
    er = tk_wai_sem(0, 1, TMO_POL);
    check(er == E_ID, "S18", "tk_wai_sem id=0 -> E_ID", er);
}

/* ---- event flags (eventflag.c) ------------------------------------------ */
static ID h_flg;
static volatile UINT h_ptn;

static void t_wait_flg_twice(INT stacd, void *exinf)
{
    UINT p = 0;
    (void)stacd; (void)exinf;
    h_er1 = tk_wai_flg(h_flg, 0x5, TWF_ANDW, &p, TMO_FEVR);
    h_ptn = p; h_flag = 1;
    h_er2 = tk_wai_flg(h_flg, 0x8, TWF_ORW, &p, TMO_FEVR);  /* released by tk_del_flg */
    h_flag = 2;
    tk_ext_tsk();
}

static void suite_flg(PRI me)
{
    ER er; ID f, t; UINT p = 0; T_RFLG rf;
    T_CFLG cf = { .exinf = NULL, .flgatr = TA_TFIFO | TA_WSGL, .iflgptn = 0 };

    f = tk_cre_flg(&cf);
    check(f > 0, "F1", "tk_cre_flg -> ID > 0", f);
    er = tk_wai_flg(f, 0x1, TWF_ANDW, &p, TMO_POL);
    check(er == E_TMOUT, "F2", "tk_wai_flg(TMO_POL) on 0 -> E_TMOUT", er);
    er = tk_wai_flg(f, 0, TWF_ANDW, &p, TMO_POL);
    check(er == E_PAR, "F3", "tk_wai_flg waiptn=0 -> E_PAR", er);
    tk_set_flg(f, 0x3);
    er = tk_wai_flg(f, 0x1, TWF_ORW | TWF_BITCLR, &p, TMO_POL);
    tk_ref_flg(f, &rf);
    check(er == E_OK && p == 0x3 && rf.flgptn == 0x2, "F4", "TWF_ORW|TWF_BITCLR returns 0x3, clears only 0x1", (W)rf.flgptn);
    er = tk_wai_flg(f, 0x3, TWF_ANDW, &p, TMO_POL);
    check(er == E_TMOUT, "F5", "TWF_ANDW 0x3 with only 0x2 set -> E_TMOUT", er);
    tk_clr_flg(f, 0);
    tk_ref_flg(f, &rf);
    check(rf.flgptn == 0, "F6", "tk_clr_flg(0) clears every bit", (W)rf.flgptn);

    /* a higher-priority AND-waiter: released only when BOTH bits are set */
    h_flg = f; h_flag = 0; h_ptn = 0; h_er1 = h_er2 = -999;
    t = mk_task(t_wait_flg_twice, me - 1);
    tk_sta_tsk(t, 0);
    tk_set_flg(f, 0x1);
    check(h_flag == 0, "F7", "AND-waiter for 0x5 still waits after 0x1", h_flag);
    er = tk_wai_flg(f, 0x2, TWF_ORW, &p, TMO_POL);
    check(er == E_OBJ, "F8", "TA_WSGL: a second waiter -> E_OBJ", er);
    tk_set_flg(f, 0x4);
    check(h_flag == 1 && h_er1 == E_OK && h_ptn == 0x5, "F9", "released by tk_set_flg before it returns (ptn 0x5)", (W)h_ptn);
    er = tk_del_flg(f);
    check(er == E_OK && h_flag == 2 && h_er2 == E_DLT, "F10", "tk_del_flg releases the waiter with E_DLT", h_er2);
    tk_del_tsk(t);
    er = tk_set_flg(f, 1);
    check(er == E_NOEXS, "F11", "tk_set_flg deleted -> E_NOEXS", er);
}

/* ---- mutexes (mutex.c) ---------------------------------------------------- */
static ID h_mtx;
static volatile ER m_erL, m_erH;

static void t_mtx_low(INT stacd, void *exinf)   /* locks, sleeps, unlocks */
{
    (void)stacd; (void)exinf;
    m_erL = tk_loc_mtx(h_mtx, TMO_POL);
    tk_slp_tsk(TMO_FEVR);
    tk_unl_mtx(h_mtx);
    tk_ext_tsk();
}

static void t_mtx_high(INT stacd, void *exinf)  /* blocks on the mutex */
{
    (void)stacd; (void)exinf;
    m_erH = tk_loc_mtx(h_mtx, TMO_FEVR);
    h_flag = 5;
    tk_unl_mtx(h_mtx);
    tk_ext_tsk();
}

static void suite_mtx(PRI me)
{
    ER er; ID m, tl, th; T_RTSK r;
    /* the protocol is ONE value, not flags: TA_TPRI|TA_INHERIT == 3 ==
     * TA_CEILING (the first draft did that and got a ceiling mutex). */
    T_CMTX cm = { .exinf = NULL, .mtxatr = TA_INHERIT, .ceilpri = 1 };

    m = tk_cre_mtx(&cm);
    check(m > 0, "M1", "tk_cre_mtx(TA_INHERIT) -> ID > 0", m);
    er = tk_loc_mtx(m, TMO_POL);
    check(er == E_OK, "M2", "tk_loc_mtx(TMO_POL) free -> E_OK", er);
    er = tk_loc_mtx(m, TMO_POL);
    check(er == E_ILUSE, "M3", "tk_loc_mtx by the owner again -> E_ILUSE", er);
    er = tk_unl_mtx(m);
    check(er == E_OK, "M4", "tk_unl_mtx by the owner -> E_OK", er);
    er = tk_unl_mtx(m);
    check(er == E_ILUSE, "M5", "tk_unl_mtx when not the owner -> E_ILUSE", er);

    /* priority inheritance: a low task holds it, a high task waits */
    h_mtx = m; h_flag = 0; m_erL = m_erH = -999;
    tl = mk_task(t_mtx_low, me + 1);
    th = mk_task(t_mtx_high, me - 1);
    tk_sta_tsk(tl, 0);
    tk_dly_tsk(20);                       /* let the low task lock + sleep */
    tk_sta_tsk(th, 0);                    /* high task blocks on the mutex */
    tk_ref_tsk(tl, &r);
    check(m_erL == E_OK && h_flag == 0 && r.tskpri == me - 1, "M6", "TA_INHERIT: holder runs at the waiter's priority", r.tskpri);
    tk_wup_tsk(tl);                       /* holder unlocks -> high task runs */
    tk_ref_tsk(tl, &r);
    check(h_flag == 5 && m_erH == E_OK, "M7", "the waiter got the mutex once it was released", m_erH);
    check(r.tskpri == me + 1, "M8", "holder is back at its own priority", r.tskpri);
    tk_dly_tsk(20);
    tk_del_tsk(tl); tk_del_tsk(th);
    er = tk_del_mtx(m);
    check(er == E_OK, "M9", "tk_del_mtx -> E_OK", er);
    er = tk_loc_mtx(m, TMO_POL);
    check(er == E_NOEXS, "M10", "tk_loc_mtx deleted -> E_NOEXS", er);
}

/* ---- message buffers (messagebuf.c) ---------------------------------------- */
static ID h_mbf;
static volatile INT h_n1, h_n2;
static UB h_rbuf[16];
/* TA_USERBUF so the same call works with or without the kernel's allocator */
static UW mbf_area[64 / sizeof(UW)];

static void t_rcv_mbf_twice(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    h_n1 = tk_rcv_mbf(h_mbf, h_rbuf, TMO_FEVR);
    h_flag = 1;
    h_n2 = tk_rcv_mbf(h_mbf, h_rbuf, TMO_FEVR);   /* released by tk_del_mbf */
    h_flag = 2;
    tk_ext_tsk();
}

static void suite_mbf(PRI me)
{
    ER er; ID b, t; INT n, n2, i, ok; T_RMBF rb;
    UB src[16], dst[16];
    T_CMBF cb = { .exinf = NULL, .mbfatr = TA_TFIFO | TA_USERBUF,
                  .bufsz = sizeof mbf_area, .maxmsz = 16, .bufptr = mbf_area };

    for (i = 0; i < 16; i++) src[i] = (UB)('a' + i);
    b = tk_cre_mbf(&cb);
    check(b > 0, "B1", "tk_cre_mbf(64 B, maxmsz 16) -> ID > 0", b);
    /* spec tk_snd_mbf: E_PAR for msgsz <= 0 and msgsz > maxmsz */
    er = tk_snd_mbf(b, src, 17, TMO_POL);
    check(er == E_PAR, "B2", "tk_snd_mbf msgsz 17 > maxmsz 16 -> E_PAR", er);
    er = tk_snd_mbf(b, src, 0, TMO_POL);
    check(er == E_PAR, "B3", "tk_snd_mbf msgsz=0 -> E_PAR", er);
    n = tk_rcv_mbf(b, dst, TMO_POL);
    check(n == E_TMOUT, "B4", "tk_rcv_mbf(TMO_POL) on empty -> E_TMOUT", n);

    /* the message is COPIED at send time: changing the source afterwards
     * does not change what is received */
    er = tk_snd_mbf(b, src, 5, TMO_POL);
    tk_ref_mbf(b, &rb);
    check(er == E_OK && rb.msgsz == 5, "B5", "tk_snd_mbf 5 B -> tk_ref_mbf msgsz 5", rb.msgsz);
    src[0] = 'Z';
    n = tk_rcv_mbf(b, dst, TMO_POL);
    check(n == 5 && dst[0] == 'a' && dst[4] == 'e', "B6", "tk_rcv_mbf -> 5 B, the bytes as sent (copied)", n);
    src[0] = 'a';

    /* FIFO order of messages */
    tk_snd_mbf(b, src, 2, TMO_POL);
    tk_snd_mbf(b, src, 3, TMO_POL);
    n = tk_rcv_mbf(b, dst, TMO_POL);
    n2 = tk_rcv_mbf(b, dst, TMO_POL);
    check(n == 2 && n2 == 3, "B7", "two messages come out in the order sent (2 B then 3 B)", n * 10 + n2);

    /* a full buffer: TMO_POL send fails with E_TMOUT instead of waiting */
    for (i = 0, ok = 0; i < 32; i++) {
        er = tk_snd_mbf(b, src, 16, TMO_POL);
        if (er != E_OK) break;
        ok++;
    }
    check(ok >= 1 && er == E_TMOUT, "B8", "tk_snd_mbf(TMO_POL) into a full buffer -> E_TMOUT", ok);
    while (tk_rcv_mbf(b, dst, TMO_POL) > 0) { }

    /* a higher-priority receiver is handed the message before tk_snd_mbf
     * returns, and is released with E_DLT by tk_del_mbf */
    h_mbf = b; h_flag = 0; h_n1 = h_n2 = -999;
    t = mk_task(t_rcv_mbf_twice, me - 1);
    tk_sta_tsk(t, 0);
    check(h_flag == 0, "B9", "higher-pri receiver blocks on an empty buffer", h_flag);
    er = tk_snd_mbf(b, src, 7, TMO_POL);
    check(er == E_OK && h_flag == 1 && h_n1 == 7, "B10", "tk_snd_mbf hands 7 B to it before returning", h_n1);
    er = tk_del_mbf(b);
    check(er == E_OK && h_flag == 2 && h_n2 == E_DLT, "B11", "tk_del_mbf releases the receiver with E_DLT", h_n2);
    tk_del_tsk(t);
    er = tk_snd_mbf(b, src, 1, TMO_POL);
    check(er == E_NOEXS, "B12", "tk_snd_mbf deleted -> E_NOEXS", er);
}

/* ---- mailboxes (mailbox.c) --------------------------------------------------- */
static ID h_mbx;
static T_MSG *volatile h_msg;
static T_MSG_PRI m_pri1, m_pri3, m_pri0;

static void t_rcv_mbx_twice(INT stacd, void *exinf)
{
    T_MSG *m = NULL;
    (void)stacd; (void)exinf;
    h_er1 = tk_rcv_mbx(h_mbx, &m, TMO_FEVR);
    h_msg = m; h_flag = 1;
    h_er2 = tk_rcv_mbx(h_mbx, &m, TMO_FEVR);   /* released by tk_del_mbx */
    h_flag = 2;
    tk_ext_tsk();
}

static void suite_mbx(PRI me)
{
    ER er, er2; ID x, t; T_MSG *m = NULL, *m2 = NULL;
    T_CMBX cx = { .exinf = NULL, .mbxatr = TA_TFIFO | TA_MPRI };

    x = tk_cre_mbx(&cx);
    check(x > 0, "X1", "tk_cre_mbx(TA_MPRI) -> ID > 0", x);
    er = tk_rcv_mbx(x, &m, TMO_POL);
    check(er == E_TMOUT, "X2", "tk_rcv_mbx(TMO_POL) on empty -> E_TMOUT", er);
    /* spec tk_snd_mbx: E_PAR for msgpri <= 0 */
    m_pri0.msgpri = 0;
    er = tk_snd_mbx(x, (T_MSG *)&m_pri0);
    check(er == E_PAR, "X3", "tk_snd_mbx msgpri=0 on a TA_MPRI box -> E_PAR", er);

    /* TA_MPRI: priority 1 is the highest and comes out first; the receiver
     * gets the SAME address that was sent (the packet is not copied) */
    m_pri3.msgpri = 3; m_pri1.msgpri = 1;
    er  = tk_snd_mbx(x, (T_MSG *)&m_pri3);
    er2 = tk_snd_mbx(x, (T_MSG *)&m_pri1);
    tk_rcv_mbx(x, &m, TMO_POL);
    tk_rcv_mbx(x, &m2, TMO_POL);
    check(er == E_OK && er2 == E_OK && m == (T_MSG *)&m_pri1, "X4", "TA_MPRI: priority 1 sent second comes out first", er);
    check(m2 == (T_MSG *)&m_pri3, "X5", "then priority 3, same address as sent", 0);

    h_mbx = x; h_flag = 0; h_msg = NULL; h_er1 = h_er2 = -999;
    t = mk_task(t_rcv_mbx_twice, me - 1);
    tk_sta_tsk(t, 0);
    check(h_flag == 0, "X6", "higher-pri receiver blocks on an empty mailbox", h_flag);
    er = tk_snd_mbx(x, (T_MSG *)&m_pri1);
    check(er == E_OK && h_flag == 1 && h_er1 == E_OK && h_msg == (T_MSG *)&m_pri1, "X7", "tk_snd_mbx hands the packet to it before returning", h_flag);
    er = tk_del_mbx(x);
    check(er == E_OK && h_flag == 2 && h_er2 == E_DLT, "X8", "tk_del_mbx releases the receiver with E_DLT", h_er2);
    tk_del_tsk(t);
    er = tk_snd_mbx(x, (T_MSG *)&m_pri1);
    check(er == E_NOEXS, "X9", "tk_snd_mbx deleted -> E_NOEXS", er);
}

/* ---- fixed-size memory pools (mempfix.c) -------------------------------------- */
static ID h_mpf;
static void *volatile h_blk;
static UW mpf_area[64 / sizeof(UW)];            /* 2 blocks of 32 B */

static void t_get_mpf_twice(INT stacd, void *exinf)
{
    void *p = NULL;
    (void)stacd; (void)exinf;
    h_er1 = tk_get_mpf(h_mpf, &p, TMO_FEVR);
    h_blk = p; h_flag = 1;
    h_er2 = tk_get_mpf(h_mpf, &p, TMO_FEVR);   /* released by tk_del_mpf */
    h_flag = 2;
    tk_ext_tsk();
}

static void suite_mpf(PRI me)
{
    ER er, er2; ID p, t; void *b1 = NULL, *b2 = NULL, *b3 = NULL; T_RMPF rp;
    UB *lo = (UB *)mpf_area, *hi = (UB *)mpf_area + sizeof mpf_area;
    T_CMPF cp = { .exinf = NULL, .mpfatr = TA_TFIFO | TA_USERBUF,
                  .mpfcnt = 2, .blfsz = 32, .bufptr = mpf_area };

    p = tk_cre_mpf(&cp);
    check(p > 0, "P1", "tk_cre_mpf(2 x 32 B) -> ID > 0", p);
    er  = tk_get_mpf(p, &b1, TMO_POL);
    er2 = tk_get_mpf(p, &b2, TMO_POL);
    check(er == E_OK && er2 == E_OK && b1 != b2
          && (UB *)b1 >= lo && (UB *)b1 < hi && (UB *)b2 >= lo && (UB *)b2 < hi,
          "P2", "two tk_get_mpf -> two different blocks inside the pool", er2);
    tk_ref_mpf(p, &rp);
    check(rp.frbcnt == 0, "P3", "tk_ref_mpf frbcnt 0 once both are out", rp.frbcnt);
    er = tk_get_mpf(p, &b3, TMO_POL);
    check(er == E_TMOUT, "P4", "tk_get_mpf(TMO_POL) on an empty pool -> E_TMOUT", er);

    /* a higher-priority waiter gets the returned block before tk_rel_mpf
     * returns; the pool itself stays empty */
    h_mpf = p; h_flag = 0; h_blk = NULL; h_er1 = h_er2 = -999;
    t = mk_task(t_get_mpf_twice, me - 1);
    tk_sta_tsk(t, 0);
    check(h_flag == 0, "P5", "higher-pri task waits on the empty pool", h_flag);
    er = tk_rel_mpf(p, b1);
    check(er == E_OK && h_flag == 1 && h_er1 == E_OK && h_blk == b1, "P6", "tk_rel_mpf hands that block to the waiter before returning", h_flag);
    tk_ref_mpf(p, &rp);
    check(rp.frbcnt == 0, "P7", "the pool is still empty (the block went to the waiter)", rp.frbcnt);
    er = tk_del_mpf(p);
    check(er == E_OK && h_flag == 2 && h_er2 == E_DLT, "P8", "tk_del_mpf releases the next waiter with E_DLT", h_er2);
    tk_del_tsk(t);
    er = tk_get_mpf(p, &b3, TMO_POL);
    check(er == E_NOEXS, "P9", "tk_get_mpf deleted -> E_NOEXS", er);
}

/* ---- cyclic handlers (time_calls.c) ------------------------------------------- */
static volatile INT c_cnt;

static void cyc_count(void *exinf)
{
    (void)exinf;
    c_cnt++;
}

static void suite_cyc(void)
{
    ER er; ID c; INT n; W t0, dt; T_RCYC rc;
    T_CCYC cc = { .exinf = NULL, .cycatr = TA_HLNG, .cychdr = (FP)cyc_count,
                  .cyctim = 20, .cycphs = 0 };
    T_CCYC bad = cc, ph = cc;

    c_cnt = 0;
    c = tk_cre_cyc(&cc);
    check(c > 0, "C1", "tk_cre_cyc(20 ms, no TA_STA) -> ID > 0", c);
    tk_dly_tsk(100);
    tk_ref_cyc(c, &rc);
    check(c_cnt == 0 && rc.cycstat == TCYC_STP, "C2", "without TA_STA it does not run (TCYC_STP)", c_cnt);
    er = tk_sta_cyc(c);
    tk_ref_cyc(c, &rc);
    check(er == E_OK && rc.cycstat == TCYC_STA, "C3", "tk_sta_cyc -> E_OK, TCYC_STA", (W)rc.cycstat);
    /* the count is judged against the time that really passed (tk_get_otm),
     * not against the 400 ms asked of tk_dly_tsk: a broken delay is D2's
     * business, not this check's (the first draft trusted the delay and went
     * red under NC-HALFDELAY too). 20 ms period -> dt/20 calls; a period that
     * doubled gives about half. */
    t0 = now_ms();
    tk_dly_tsk(400);
    n = c_cnt;
    dt = now_ms() - t0;
    check(n * 20 >= dt * 7 / 10 && n * 20 <= dt * 3 / 2, "C4", "20 ms period -> about dt/20 calls (0.7x..1.5x)", n);
    er = tk_stp_cyc(c);
    n = c_cnt;
    tk_dly_tsk(100);
    check(er == E_OK && c_cnt == n, "C5", "tk_stp_cyc -> no more calls", c_cnt - n);
    er = tk_del_cyc(c);
    check(er == E_OK && tk_ref_cyc(c, &rc) == E_NOEXS, "C6", "tk_del_cyc -> E_OK, then tk_ref_cyc -> E_NOEXS", er);
    /* the spec does not say that ID 0 is invalid; E_ID for it is the
     * reference kernel's reading (CHECK_CYCID: 0 is below the ID range) */
    er = tk_sta_cyc(0);
    check_impl(er == E_ID, "C7", "[impl] tk_sta_cyc id=0 -> E_ID", er);
    /* spec (tk_cre_cyc): "cyctim に0を指定することはできない", and E_PAR
     * lists cyctim — so this one is the spec's, not [impl] (audit-17) */
    bad.cyctim = 0;
    er = tk_cre_cyc(&bad);
    check(er == E_PAR, "C8", "tk_cre_cyc cyctim=0 -> E_PAR", er);
    if (er > 0) tk_del_cyc(er);
    /* the start phase (spec: the n-th call comes at least cycphs + cyctim *
     * (n - 1) after tk_cre_cyc — a lower bound only). TA_STA, period 300,
     * phase 250: no call at about 100 ms, exactly one at about 400 ms (the
     * first after 250, the second not before 550). That the first call has
     * come by ~400 ms is a timeliness assumption, the same kind C4 makes;
     * audit-18 measured that a first call 160 ms late still passes. */
    c_cnt = 0;
    ph.cycatr = TA_HLNG | TA_STA; ph.cyctim = 300; ph.cycphs = 250;
    t0 = now_ms();
    c = tk_cre_cyc(&ph);
    tk_dly_tsk(100);
    n = c_cnt;
    check(c > 0 && n == 0, "C9", "TA_STA, cycphs=250: no call before the phase (at ~100 ms)", n);
    tk_dly_tsk(300);
    n = c_cnt;
    dt = now_ms() - t0;
    check(n == 1 && dt >= 260 && dt < 550, "C10", "TA_STA, cycphs=250, cyctim=300: exactly one call at ~400 ms", n);
    if (c > 0) tk_del_cyc(c);
}

/* ---- time ----------------------------------------------------------------- */
static void suite_time(void)
{
    ER er; W t0, dt;
    /* 200 ms, not 30: with a 10 ms tick a delay that is silently halved
     * still rounds up past 30 ms (NC-HALFDELAY stayed green at 30). */
    t0 = now_ms();
    er = tk_dly_tsk(200);
    dt = now_ms() - t0;
    check(er == E_OK, "D1", "tk_dly_tsk(200) -> E_OK", er);
    check(dt >= 200 && dt < 2000, "D2", "tk_dly_tsk(200) lasted >= 200 ms", dt);
    er = tk_dly_tsk(0);
    check(er == E_OK, "D3", "tk_dly_tsk(0) -> E_OK", er);
    t0 = now_ms(); tk_dly_tsk(10);
    check(now_ms() - t0 >= 10, "D4", "tk_get_otm advances across a 10 ms delay", now_ms() - t0);
}

/* ---- dispatch disable (cpuctl.c, misc_calls.c) ----------------------------- *
 * spec (tk_dis_dsp): while disabled, a higher-priority task made READY by the *
 * caller or by an interrupt handler is not dispatched until tk_ena_dsp; a     *
 * call that may wait returns E_CTX; tk_ref_sys shows TSS_DDSP; tk_dis_dsp     *
 * twice is ended by one tk_ena_dsp. Nothing is printed while disabled (the    *
 * output path is not part of the promise), so the values are kept and checked *
 * after tk_ena_dsp. In K7 the only dispatch points are interrupt exits: on    *
 * bare x86 that is the IRQ-exit dispatch of RNG0 (knl_irq_exit_dispatch must  *
 * honour the disable). So under NC-NODDS K7 goes red only on a kernel that    *
 * dispatches at interrupt exit: measured 2026-09-28, bare x86 with the RNG0   *
 * fix only — not bare x86 before it, not bare AArch64, not hosted Linux (K2.. *
 * K5 still make that NC red everywhere).                                      */
static void t_delay_then_flag(INT stacd, void *exinf)
{
    (void)exinf;
    tk_dly_tsk(20);
    h_flag = stacd;
    tk_ext_tsk();
}

static void suite_dds(PRI me)
{
    ER er_dis, er_slp, er_ena; ID t, runid, schedid; T_RSYS rs;
    UINT st_dis, st_ena; INT f_dis, f_ena; W t0, dt; volatile UW spin = 0;

    t = mk_task(t_sleep_then_exit, me - 1);
    h_flag = 0; h_er1 = -999;
    tk_sta_tsk(t, 0);                     /* runs at once, then sleeps */
    er_dis = tk_dis_dsp();
    tk_ref_sys(&rs); st_dis = rs.sysstat;
    tk_wup_tsk(t);
    f_dis = h_flag;
    tk_ref_sys(&rs); runid = rs.runtskid; schedid = rs.schedtskid;
    er_slp = tk_slp_tsk(10);
    tk_dis_dsp();                         /* twice: one tk_ena_dsp still ends it */
    er_ena = tk_ena_dsp();
    f_ena = h_flag;
    tk_ref_sys(&rs); st_ena = rs.sysstat;
    check(er_dis == E_OK, "K1", "tk_dis_dsp -> E_OK", er_dis);
    check((st_dis & TSS_DDSP) != 0, "K2", "tk_ref_sys shows TSS_DDSP while disabled", (W)st_dis);
    check(f_dis == 0, "K3", "a higher-pri task woken while disabled does not run", f_dis);
    check(runid == tk_get_tid() && schedid == t, "K4", "tk_ref_sys: runtskid = me, schedtskid = the woken task", schedid);
    check(er_slp == E_CTX, "K5", "tk_slp_tsk while disabled -> E_CTX", er_slp);
    check(er_ena == E_OK && f_ena == 1 && (st_ena & TSS_DDSP) == 0, "K6",
          "tk_dis_dsp twice, tk_ena_dsp once -> enabled; the woken task runs before it returns", f_ena);
    tk_del_tsk(t);

    /* K7's busy wait makes NO kernel call, so the only dispatch points are
     * interrupt exits (a loop that polled tk_get_otm would test the service-
     * call exit instead — the first draft did). Calibrate the loop first:
     * how many 64k-iteration chunks fit in ~50 ms, then spin 3x that. */
    {
        UW chunks = 0, i;
        t0 = now_ms();
        while (now_ms() - t0 < 50 && chunks < 20000) {   /* 3x stays < 2^32 */
            for (i = 0; i < 65536; i++) spin++;
            chunks++;
        }
        t = mk_task(t_delay_then_flag, me - 1);
        h_flag = 0;
        tk_sta_tsk(t, 2);                 /* runs at once, sleeps 20 ms */
        tk_dis_dsp();
        t0 = now_ms();
        for (i = 0; i < chunks * 3 * 65536; i++) spin++;
        f_dis = h_flag;                   /* read before any kernel call */
        dt = now_ms() - t0;
    }
    tk_ena_dsp();
    f_ena = h_flag;
    check(f_dis == 0 && dt >= 40, "K7", "a task whose 20 ms delay ends while disabled does not run (~150 ms busy, no kernel call)", f_dis);
    check(f_ena == 2, "K8", "... and runs as soon as tk_ena_dsp", f_ena);
    tk_del_tsk(t);
}

/* The suite runs in its own task at TKC_PRI, so helpers can sit one above
 * and one below it whatever the caller's priority is (the hosted shell runs
 * at 1). The caller sleeps until the runner wakes it — tk_slp_tsk/tk_wup_tsk,
 * not a semaphore, so a negative control that breaks semaphores breaks the
 * checks, not the harness. */
#define TKC_PRI 20
static ID g_caller;
static volatile INT g_done;

static void t_runner(INT stacd, void *exinf)
{
    T_RTSK r;
    (void)stacd; (void)exinf;
    tk_ref_tsk(TSK_SELF, &r);
    suite_task(r.tskpri);
    suite_sem(r.tskpri);
    suite_flg(r.tskpri);
    suite_mtx(r.tskpri);
    suite_mbf(r.tskpri);
    suite_mbx(r.tskpri);
    suite_mpf(r.tskpri);
    suite_cyc();
    suite_time();
    suite_dds(r.tskpri);
    g_done = 1;
    tk_wup_tsk(g_caller);
    tk_ext_tsk();
}

INT tk_conform_run(void (*out)(const char *))
{
    char b[64]; INT k = 0; const char *p;
    ID run;
    g_out = out; g_pass = g_fail = 0; g_done = 0;
    out("[tkc] contract suite v2 (runner task pri 20)\r\n");
    g_caller = tk_get_tid();
    run = mk_task(t_runner, TKC_PRI);
    if (run <= 0) {
        out("[tkc] FAIL setup: could not create the runner task\r\n");
        return 1;
    }
    tk_can_wup(TSK_SELF);
    tk_sta_tsk(run, 0);
    while (!g_done && tk_slp_tsk(120000) == E_OK) { }
    if (!g_done) {
        out("[tkc] FAIL setup: the runner did not finish within 120 s\r\n");
        g_fail++;
        tk_ter_tsk(run);
    }
    tk_del_tsk(run);
    k = 0;
    for (p = "[tkc] "; *p; ) b[k++] = *p++;
    put_dec(b, &k, g_pass);
    for (p = " PASS / "; *p; ) b[k++] = *p++;
    put_dec(b, &k, g_fail);
    for (p = " FAIL\r\n"; *p; ) b[k++] = *p++;
    b[k] = 0;
    out(b);
    return g_fail;
}
