/*
 *  pool_preempt.c — `tkconf pool`: the shared matmul pool under the hosted
 *  exit dispatch (audit-21 found it split; the probe is pk-scratch/aud21-hed-
 *  probe.py, this is its test form).
 *
 *  A runner task (pri 20) drives pk_parallel_rows 150 times with slow rows
 *  while a higher task (pri 19) wakes every 3 ms and drives the same pool 40
 *  times. Every row of every call must come back with that call's value.
 *    [pool] PASS P1  no wrong rows in the low task's calls
 *    [pool] PASS P2  no wrong rows in the high task's calls
 *    [pool] PASS P3  the high task finished its 40 calls
 *    [pool] PASS P4  precondition: the pool really ran in parallel (helper wakes)
 *    [pool] PASS P5  no starvation: high calls started inside the low loop
 *  Negative controls: -DPK_POOL_PREEMPTIBLE (busy not counted) turns P1 and
 *  P2 red; -DPK_POOL_NO_LEAVE_DISPATCH (no switch when the pool is left) and
 *  -DPK_HOSTED_NO_EXIT_DISPATCH (the old behaviour, RNG0) turn P5 red. Needs
 *  >= 2 CPUs (else P4 is red: the pool runs inline and proves nothing).
 *  `tkconf nosw` (N1-) and `tkconf fp` (F1-F2) live here too: see each probe.
 */
#include "kernel.h"
#include "kdds.h"
#include "pfs_block.h"
#include "../common/llm/pk_parallel.h"
#include "../common/llm/student.h"
#include "arkfs.h"
#include "dtr.h"
#include "../common/llm/dlb.h"
#include "dkva.h"
#include "pk_nosw.h"
#include <stdio.h>
#include <string.h>

#define PP_ROWS   512
#define PP_LCALLS 150
#define PP_HCALLS 40

struct pp_ctx { volatile UW *buf; UW val; UW work; };
static volatile UW pp_L[PP_ROWS], pp_H[PP_ROWS];
static volatile UW pp_slow;
static volatile INT pp_h_runs, pp_h_bad, pp_h_during, pp_l_active;

static void pp_body(void *ctx, size_t i0, size_t i1)
{
    struct pp_ctx *c = ctx;
    for (size_t i = i0; i < i1; i++) {
        for (UW k = 0; k < c->work; k++) pp_slow++;
        c->buf[i] = c->val;
    }
}

static void t_pp_high(INT stacd, void *exinf)
{
    (void)exinf;
    for (INT n = 0; n < stacd; n++) {
        struct pp_ctx c = { pp_H, 7000 + (UW)n, 200 };
        tk_dly_tsk(3);
        for (INT i = 0; i < PP_ROWS; i++) pp_H[i] = 0;
        if (pp_l_active) pp_h_during++;
        pk_parallel_rows(PP_ROWS, pp_body, &c);
        INT bad = 0;
        for (INT i = 0; i < PP_ROWS; i++) if (pp_H[i] != c.val) bad++;
        if (bad) pp_h_bad++;
        pp_h_runs++;
    }
    tk_ext_tsk();
}

static ID pp_mk(void (*fn)(INT, void *), PRI pri)
{
    T_CTSK ct = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                  .task = (FP)fn, .itskpri = pri, .stksz = 8192 };
    return tk_cre_tsk(&ct);
}

static void (*pp_out)(const char *);
/* arch/linux/aarch64/preempt.c: SIGALRM ticks that arrived on a thread other
 * than the T-Kernel one (not switched). Weak: absent on x86_64. */
extern volatile unsigned long arch_preempt_foreign_ticks __attribute__((weak));
static const char *pp_tag = "[pool]";
static INT pp_fail;
static INT pp_skip;
static ID pp_caller;
static volatile INT pp_done;

static void pp_check(int ok, const char *id, const char *what, long got)
{
    char b[200];
    snprintf(b, sizeof b, "%s %s %s  %s (got %ld)\r\n", pp_tag, ok ? "PASS" : "FAIL", id, what, got);
    pp_out(b);
    if (!ok) pp_fail++;
}

static void t_pp_runner(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    INT lbad = 0;
    unsigned long w0 = pk_parallel_wake_count();
    pp_h_runs = pp_h_bad = pp_h_during = 0;
    ID t = pp_mk(t_pp_high, 19);
    if (t > 0) tk_sta_tsk(t, PP_HCALLS);
    pp_l_active = 1;
    for (INT n = 0; n < PP_LCALLS; n++) {
        struct pp_ctx c = { pp_L, 1000 + (UW)n, 4000 };
        for (INT i = 0; i < PP_ROWS; i++) pp_L[i] = 0;
        pk_parallel_rows(PP_ROWS, pp_body, &c);
        INT bad = 0;
        for (INT i = 0; i < PP_ROWS; i++) if (pp_L[i] != c.val) bad++;
        if (bad) lbad++;
    }
    pp_l_active = 0;
    for (INT n = 0; n < 200 && pp_h_runs < PP_HCALLS; n++) tk_dly_tsk(10);
    unsigned long wakes = pk_parallel_wake_count() - w0;
    pp_check(lbad == 0, "P1", "low task: calls with a wrong row (of 150)", lbad);
    pp_check(pp_h_bad == 0, "P2", "high task: calls with a wrong row", pp_h_bad);
    pp_check(pp_h_runs == PP_HCALLS, "P3", "high task: calls finished (of 40)", pp_h_runs);
    pp_check(wakes > 0, "P4", "precondition: pool helper wakes", (long)wakes);
    pp_check(pp_h_during > 0, "P5", "no starvation: high calls started inside the low loop", pp_h_during);
    if (&arch_preempt_foreign_ticks) {          /* hosted aarch64 only (weak) */
        char m[96];
        snprintf(m, sizeof m, "[pool] note: ticks taken on another OS thread %lu\r\n",
                 (unsigned long)arch_preempt_foreign_ticks);
        pp_out(m);
    }
    if (t > 0) { tk_ter_tsk(t); tk_del_tsk(t); }
    pp_done = 1;
    tk_wup_tsk(pp_caller);
    tk_ext_tsk();
}

/* ---- `tkconf nosw`: a kdds topic slot under task switches (pk_nosw.h) ----
 * The runner (pri 20) reads one LATEST_ONLY topic over and over and checks
 * that all 192 bytes are one value; a higher task (pri 19) wakes every 2 ms
 * and publishes a new value to the same topic, 60 times.
 *   N1  no torn read (a read with two values in it)
 *   N2  the high task finished its 60 publishes
 *   N3  no starvation: publishes landed while the runner was reading
 *   N4  every p-fs block put by two tasks at once reads back whole
 *   N5  no starvation: high puts started during the runner's puts (asked
 *       only with -DPFS_FAULT_SLOW_PUT: unwidened, the puts end too soon)
 *   N6-N8 the student (ns_student below), N9-N11 ARK (ns_ark below),
 *   N12-N13 dtr (ns_dtr below), N14-N15 the dlb trace ring (ns_dlb below),
 *   N16-N17 the dkva KV cache (ns_dkva below), N18-N19 the pool inside a
 *   region (ns_poolregion below), N20-N22 net_lan's send buffer and nonce
 *   (ns_netlan below)
 * -DPK_NOSW_OFF (empty regions) must turn N1 red, and N4 red when built with
 * -DPFS_FAULT_SLOW_PUT=2000000 too (green with only the latter). */
#define NS_LEN   192                  /* KDDS_DATA_MAX */
#define NS_PUBS  60
static volatile INT ns_pubs, ns_during, ns_reading;

static void t_ns_high(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    UB b[NS_LEN];
    W h = kdds_open_poll_scoped("tkc/nosw", KDDS_QOS_LATEST_ONLY, KDDS_SCOPE_GLOBAL);
    for (INT n = 0; n < NS_PUBS && h >= 0; n++) {
        tk_dly_tsk(2);
        for (INT i = 0; i < NS_LEN; i++) b[i] = (UB)(2 + n % 250);
        if (ns_reading) ns_during++;
        kdds_pub(h, b, NS_LEN);
        ns_pubs++;
    }
    if (h >= 0) kdds_close(h);
    tk_ext_tsk();
}

/* N4/N5: p-fs puts from two tasks. The runner puts 20 blocks while a pri-19
 * task puts 20 more, one every 2 ms; then every block must read back whole.
 * The pick-a-free-slot -> mark-used window is short, so the test builds widen
 * it with -DPFS_FAULT_SLOW_PUT=<spins>; without it N4 proves little (said). */
#define NP_N 20
static U1 np_idh[NP_N][PFS_ID_LEN];
static volatile INT np_hput, np_during, np_putting;
static void np_fill(UB *b, UB who, INT i)
{
    for (INT k = 0; k < 64; k++) b[k] = (UB)(who ^ (i * 7) ^ (k * 13));
    b[0] = who; b[1] = (UB)i;
}
static void t_np_high(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    UB b[64];
    for (INT n = 0; n < NP_N; n++) {
        tk_dly_tsk(2);
        np_fill(b, 0xB0, n);
        if (np_putting) np_during++;
        pfs_put(b, 64, np_idh[n]);
        np_hput++;
    }
    tk_ext_tsk();
}
static void ns_pfs(void)
{
    static U1 idl[NP_N][PFS_ID_LEN];
    UB b[64], r[64];
    INT lost = 0, full = 0;
    np_hput = np_during = 0;
    ID t = pp_mk(t_np_high, 19);
    if (t > 0) tk_sta_tsk(t, 0);
    np_putting = 1;
    for (INT i = 0; i < NP_N; i++) {
        np_fill(b, 0xA0, i);
        if (pfs_put(b, 64, idl[i]) == PFS_E_FULL) full++;
    }
    np_putting = 0;
    for (INT n = 0; n < 100 && np_hput < NP_N; n++) tk_dly_tsk(10);
    for (INT i = 0; i < NP_N; i++) {
        np_fill(b, 0xA0, i);
        if (pfs_get(idl[i], r, 64) != 64 || memcmp(r, b, 64)) lost++;
        np_fill(b, 0xB0, i);
        if (pfs_get(np_idh[i], r, 64) != 64 || memcmp(r, b, 64)) lost++;
    }
    { char m[200];
      snprintf(m, sizeof m, "[nosw] p-fs: %d + %d puts, table full %d times, window widened: %s\r\n",
               NP_N, (int)np_hput, (int)full,
#ifdef PFS_FAULT_SLOW_PUT
               "yes"
#else
               "no"
#endif
               );
      pp_out(m); }
    pp_check(lost == 0, "N4", "p-fs blocks that do not read back whole (of 40)", lost);
#ifdef PFS_FAULT_SLOW_PUT
    pp_check(np_during > 0, "N5", "no starvation: high puts during the runner's puts", np_during);
#else
    /* unwidened, the runner's 20 puts end before the high task first wakes
     * (measured: 0), so N4 above proves little and N5 is not asked */
    pp_out("[nosw] note: N5 needs -DPFS_FAULT_SLOW_PUT (not asked in this build)\r\n");
#endif
    if (t > 0) { tk_ter_tsk(t); tk_del_tsk(t); }
}

/* N6-N8: the student (pk_nosw.h regions in student.c, ST_TRAIN_STEP at the
 * callers). The runner evaluates, then trains, one model while a pri-19 task
 * wakes every 2 ms and runs st_forward on the SAME model (as the chat or a
 * canon probe on g_student does while the DMN trains it), 60 times a phase.
 * st_forward frees and rebuilds m->cache, which the runner's eval / backward
 * read, and shares tw_buf across models.
 *   N6  every eval of the unchanged model returns the bits it did alone
 *   N7  the trained weights equal a twin trained the same number of steps
 *       alone (bit for bit)
 *   N8  no starvation: the high forwards landed while the runner was busy
 * -DPK_NOSW_OFF must turn N6 and N7 red. */
#define NT_N  32
#define NT_HF 60
static st_model nt_m, nt_ref;
static uint8_t nt_a[NT_N], nt_b[NT_N];
static float nt_hl[NT_N * ST_VOCAB], nt_lg[NT_N * ST_VOCAB];
static volatile INT nt_hrun, nt_during, nt_busy, nt_heval_on;
static volatile float nt_heval;      /* N6 premise: an eval in the high task */

static void t_nt_high(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    for (INT n = 0; n < NT_HF; n++) {
        tk_dly_tsk(2);
        if (nt_busy) nt_during++;
        if (nt_heval_on && n == 0) { int hp; nt_heval = st_eval_loss(&nt_m, nt_a, NT_N, &hp); }
        st_forward(&nt_m, nt_b, NT_N, nt_hl);
        nt_hrun++;
    }
    tk_ext_tsk();
}

static void nt_phase_end(ID t)
{
    nt_busy = 0;
    for (INT n = 0; n < 200 && nt_hrun < NT_HF; n++) tk_dly_tsk(10);
    if (t > 0) { tk_ter_tsk(t); tk_del_tsk(t); }
}

static void ns_student(void)
{
    for (INT i = 0; i < NT_N; i++) {
        nt_a[i] = (uint8_t)('a' + (i * 7) % 26);
        nt_b[i] = (uint8_t)('0' + (i * 3) % 10);
    }
    if (st_init(&nt_m, 7) != ST_OK || st_init(&nt_ref, 7) != ST_OK) {
        pp_out("[nosw] FAIL setup: st_init\r\n"); pp_fail++; return;
    }
    int np = 0;
    float l0 = st_eval_loss(&nt_m, nt_a, NT_N, &np);
    float l1 = st_eval_loss(&nt_m, nt_a, NT_N, &np);
    float l2 = (st_forward(&nt_m, nt_b, NT_N, nt_hl), st_eval_loss(&nt_m, nt_a, NT_N, &np));
    float l3 = (tk_dly_tsk(20), st_eval_loss(&nt_m, nt_a, NT_N, &np));
    float l4 = l3;
    { SYSTIM t0, t1; tk_get_tim(&t0);
      do { l4 = st_eval_loss(&nt_m, nt_a, NT_N, &np); tk_get_tim(&t1); }
      while (!memcmp(&l4, &l0, sizeof l4) && (t1.lo - t0.lo) < 300); }
    float lbad = l0;
    INT during = 0, evals = 0, bad = 0, steps = 0, diff = 0;

    nt_hrun = nt_during = 0; nt_heval_on = 1; nt_heval = 0.0f;
    ID t = pp_mk(t_nt_high, 19);
    if (t > 0) tk_sta_tsk(t, 0);
    nt_busy = 1;
    while (nt_hrun < NT_HF && evals < 200000) {
        float l = st_eval_loss(&nt_m, nt_a, NT_N, &np);
        evals++;
        if (memcmp(&l, &l0, sizeof l)) { if (!bad) lbad = l; bad++; }
    }
    nt_phase_end(t);
    during += nt_during;
    nt_heval_on = 0;
    float l5 = st_eval_loss(&nt_m, nt_a, NT_N, &np);
    float lh = nt_heval;

    nt_hrun = nt_during = 0;
    t = pp_mk(t_nt_high, 19);
    if (t > 0) tk_sta_tsk(t, 0);
    nt_busy = 1;
    while (nt_hrun < NT_HF && steps < 100000) {
        ST_TRAIN_STEP(&nt_m, nt_a, NT_N, nt_lg, 0.01f);
        steps++;
    }
    nt_phase_end(t);
    during += nt_during;
    for (INT s = 0; s < steps; s++) ST_TRAIN_STEP(&nt_ref, nt_a, NT_N, nt_lg, 0.01f);
    for (INT i = 0; i < nt_m.n_params; i++)
        if (memcmp(&nt_m.w[i], &nt_ref.w[i], sizeof(float))) diff++;

    { char m[200];
      snprintf(m, sizeof m, "[nosw] student: %d evals, %d train steps, %d params\r\n",
               (int)evals, (int)steps, nt_m.n_params);
      pp_out(m);
      UW b0, b1, b2, b3, b4, bb, b5, bh;
      memcpy(&b5, &l5, 4); memcpy(&bh, &lh, 4);
      memcpy(&b0, &l0, 4); memcpy(&b1, &l1, 4); memcpy(&b2, &l2, 4); memcpy(&bb, &lbad, 4);
      memcpy(&b3, &l3, 4); memcpy(&b4, &l4, 4);
      snprintf(m, sizeof m, "[nosw] student: eval bits alone %08x %08x, after another forward %08x, "
               "after a 20 ms sleep %08x, 300 ms of evals alone %08x, first changed %08x\r\n",
               (unsigned)b0, (unsigned)b1, (unsigned)b2, (unsigned)b3, (unsigned)b4, (unsigned)bb);
      pp_out(m);
      snprintf(m, sizeof m, "[nosw] student: eval in the high task %08x, alone after the phase %08x\r\n",
               (unsigned)bh, (unsigned)b5);
      pp_out(m); }
    pp_check(bad == 0, "N6", "evals of the unchanged model that changed", bad);
    pp_check(diff == 0, "N7", "weights unlike the twin trained alone", diff);
    pp_check(during > 0, "N8", "no starvation: high forwards while the runner was busy", during);
    st_free(&nt_m); st_free(&nt_ref);
}

/* N9-N11: ARK (pk_nosw.h regions in arkfs.c). On a RAM device of its own the
 * runner puts 200 blocks, then gets and checks them over and over, while a
 * pri-19 task wakes every 2 ms and puts 60 more. Every public ARK entry shares
 * g_blkbuf / g_payld / g_secbuf, the log head and the index.
 *   N9   no get returned another block's bytes or failed, no put failed
 *   N10  afterwards every block of both tasks reads back whole
 *   N11  no starvation: high puts landed while the runner was busy
 * Skipped (said) when p-fs runs on ARK (PKERNEL_PFS_BACKEND=ark): the probe
 * would unmount the live log. -DPK_NOSW_OFF must turn N9 or N10 red; unwidened
 * that was 1 of 2 runs (/build/r1006k), so test builds widen the get with
 * -DARK_FAULT_SLOW_GET=<spins> (arkfs.c read_record), as N4 does for p-fs. */
extern int pfs_ark_configured(void);          /* arch/linux/pfs_ark.c */
#define NA_SEC 4096                   /* 2 MiB: 260 blocks of 2 KiB + log */
#define NA_LEN 2048                   /* 256 B crossed 1 of 2 with -DPK_NOSW_OFF */
#define NA_RUN 200
#define NA_HI  60
static U1 na_ram[NA_SEC * ARK_SECTOR];
static U1 na_idr[NA_RUN][ARK_ID_LEN], na_idh[NA_HI][ARK_ID_LEN];
static volatile INT na_hput, na_hbad, na_during, na_busy;

static INT na_read(void *ctx, U4 lba, U4 n, void *buf)
{
    (void)ctx;
    if (lba + n > NA_SEC) return -1;
    memcpy(buf, na_ram + (size_t)lba * ARK_SECTOR, (size_t)n * ARK_SECTOR);
    return 0;
}
static INT na_write(void *ctx, U4 lba, U4 n, const void *buf)
{
    (void)ctx;
    if (lba + n > NA_SEC) return -1;
    memcpy(na_ram + (size_t)lba * ARK_SECTOR, buf, (size_t)n * ARK_SECTOR);
    return 0;
}
static void na_fill(UB *b, UB who, INT i)
{
    for (INT k = 0; k < NA_LEN; k++) b[k] = (UB)(who ^ (i * 7) ^ (k * 13));
    b[0] = who; b[1] = (UB)i; b[2] = (UB)(i >> 8);
}
static void t_na_high(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    static UB b[NA_LEN];
    for (INT n = 0; n < NA_HI; n++) {
        tk_dly_tsk(2);
        na_fill(b, 0xB0, n);
        if (na_busy) na_during++;
        if (ark_block_put(b, NA_LEN, na_idh[n]) != ARK_OK) na_hbad++;
        na_hput++;
    }
    tk_ext_tsk();
}
static void ns_ark(void)
{
    if (pfs_ark_configured()) {
        pp_out("[nosw] note: N9-N11 skipped (p-fs runs on ARK here)\r\n");
        return;
    }
    static ARK_BDEV bd = { ARK_SECTOR, NA_SEC, na_read, na_write, 0, 0 };
    if (ark_format(&bd) != ARK_OK || ark_mount(&bd) != ARK_OK) {
        pp_out("[nosw] FAIL setup: ark format / mount\r\n"); pp_fail++; return;
    }
    static UB b[NA_LEN], r[NA_LEN];   /* off the 8 KB task stack */
    INT nr = 0, gets = 0, bad = 0, lost = 0;
    na_hput = na_hbad = na_during = 0;
    ID t = pp_mk(t_na_high, 19);
    if (t > 0) tk_sta_tsk(t, 0);
    na_busy = 1;
    for (INT it = 0; it < 2000000 && na_hput < NA_HI; it++) {
        if (nr < NA_RUN) {
            na_fill(b, 0xA0, nr);
            if (ark_block_put(b, NA_LEN, na_idr[nr]) != ARK_OK) bad++;
            nr++;
        } else {
            INT i = it % NA_RUN;
            na_fill(b, 0xA0, i);
            if (ark_block_get(na_idr[i], r, NA_LEN) != NA_LEN || memcmp(r, b, NA_LEN)) bad++;
            gets++;
        }
    }
    na_busy = 0;
    for (INT n = 0; n < 200 && na_hput < NA_HI; n++) tk_dly_tsk(10);
    if (t > 0) { tk_ter_tsk(t); tk_del_tsk(t); }
    for (INT i = 0; i < nr; i++) {
        na_fill(b, 0xA0, i);
        if (ark_block_get(na_idr[i], r, NA_LEN) != NA_LEN || memcmp(r, b, NA_LEN)) lost++;
    }
    for (INT i = 0; i < na_hput; i++) {
        na_fill(b, 0xB0, i);
        if (ark_block_get(na_idh[i], r, NA_LEN) != NA_LEN || memcmp(r, b, NA_LEN)) lost++;
    }
    ark_unmount();
    { char m[200];
      snprintf(m, sizeof m, "[nosw] ark: %d + %d puts (%d high puts failed), %d gets, get widened: %s\r\n",
               (int)nr, (int)na_hput, (int)na_hbad, (int)gets,
#ifdef ARK_FAULT_SLOW_GET
               "yes"
#else
               "no"
#endif
               );
      pp_out(m); }
    pp_check(bad == 0 && na_hbad == 0, "N9", "ark gets / puts that failed or crossed", bad + na_hbad);
    pp_check(lost == 0, "N10", "ark blocks that do not read back whole", lost);
    pp_check(na_during > 0, "N11", "no starvation: high puts while the runner was busy", na_during);
}

/* N12-N13: dtr (pk_nosw.h regions in dtr.c). The runner calls
 * dtr_forward_probs on one input over and over while a pri-19 task wakes every
 * 2 ms and swaps the weights between two sets A and B (dtr_weights_set, as the
 * gossip average and spec.c do), 60 times. Every result must be exactly A's or
 * exactly B's, never a mix. The weights are put back afterwards.
 *   N12  no result that is neither A's nor B's
 *   N13  no starvation: swaps landed while the runner was busy
 * -DPK_NOSW_OFF must turn N12 red. */
#define ND_HI 60
static float nd_w[2][DTR_WEIGHT_FLOATS], nd_save[DTR_WEIGHT_FLOATS];
static volatile INT nd_hrun, nd_during, nd_busy;
static void t_nd_high(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    for (INT n = 0; n < ND_HI; n++) {
        tk_dly_tsk(2);
        if (nd_busy) nd_during++;
        dtr_weights_set(nd_w[(n + 1) & 1]);
        nd_hrun++;
    }
    tk_ext_tsk();
}
static void ns_dtr(void)
{
    const B in[DTR_SEQ_LEN] = { 12, -40, 77, 5 };
    float pa[DTR_OUT_DIM], pb[DTR_OUT_DIM], p[DTR_OUT_DIM];
    dtr_weights_get(nd_save);
    dtr_weights_get(nd_w[0]);
    dtr_reinit_weights(0x5EEDu);
    dtr_weights_get(nd_w[1]);
    dtr_weights_set(nd_w[0]); dtr_forward_probs(in, pa);
    dtr_weights_set(nd_w[1]); dtr_forward_probs(in, pb);
    dtr_weights_set(nd_w[0]);
    INT calls = 0, mixed = 0;
    nd_hrun = nd_during = 0;
    ID t = pp_mk(t_nd_high, 19);
    if (t > 0) tk_sta_tsk(t, 0);
    nd_busy = 1;
    while (nd_hrun < ND_HI && calls < 20000000) {
        dtr_forward_probs(in, p);
        calls++;
        if (memcmp(p, pa, sizeof p) && memcmp(p, pb, sizeof p)) mixed++;
    }
    nd_busy = 0;
    for (INT n = 0; n < 200 && nd_hrun < ND_HI; n++) tk_dly_tsk(10);
    if (t > 0) { tk_ter_tsk(t); tk_del_tsk(t); }
    dtr_weights_set(nd_save);
    { char m[200];
      snprintf(m, sizeof m, "[nosw] dtr: %d forwards, %d weight swaps, A and B differ: %s\r\n",
               (int)calls, (int)nd_hrun, memcmp(pa, pb, sizeof pa) ? "yes" : "NO");
      pp_out(m); }
    pp_check(mixed == 0 && memcmp(pa, pb, sizeof pa), "N12",
             "dtr results that were neither A's nor B's", mixed);
    pp_check(nd_during > 0, "N13", "no starvation: swaps while the runner was busy", nd_during);
}

/* N14-N15: the dlb trace ring (pk_nosw.h regions in dlb.c). As the DMN does,
 * the runner gc-packs the ring over and over (after enqueueing one UNverified
 * trace each time, which gc removes); as the chat does, a pri-19 task wakes
 * every 2 ms and enqueues one verified trace, 50 times (ring max 64). Then the
 * ring must hold exactly 50 verified traces (contents are not compared).
 *   N14  no verified trace lost
 *   N15  no starvation: enqueues landed while the runner was busy
 * The ring is reset before and after (pending live traces are dropped: this is
 * a test command; a DMN sleep tick in the window could distill them into the
 * live student). -DPK_NOSW_OFF must turn N14 red. */
#define NL_HI 50
static volatile INT nl_hrun, nl_during, nl_busy;
static void nl_q(uint8_t *q, INT i) { q[0] = 'q'; q[1] = (uint8_t)('A' + i % 26); q[2] = (uint8_t)i; }
static void t_nl_high(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    uint8_t q[3], a[8] = { 'a', 'n', 's', 'w', 'e', 'r', '!', 0 };
    for (INT n = 0; n < NL_HI; n++) {
        tk_dly_tsk(2);
        if (nl_busy) nl_during++;
        nl_q(q, n); a[7] = (uint8_t)n;
        dlb_compound_enqueue(q, 3, a, 8, 1);
        nl_hrun++;
    }
    tk_ext_tsk();
}
static void ns_dlb(void)
{
    const uint8_t junk[4] = { 'j', 'u', 'n', 'k' };
    dlb_compound_reset();
    nl_hrun = nl_during = 0;
    ID t = pp_mk(t_nl_high, 19);
    if (t > 0) tk_sta_tsk(t, 0);
    nl_busy = 1;
    INT loops = 0;
    while (nl_hrun < NL_HI && loops < 1000000000) {
        dlb_compound_enqueue(junk, 2, junk + 2, 2, 0);
        dlb_compound_gc();
        loops++;
    }
    nl_busy = 0;
    for (INT n = 0; n < 200 && nl_hrun < NL_HI; n++) tk_dly_tsk(10);
    if (t > 0) { tk_ter_tsk(t); tk_del_tsk(t); }
    dlb_compound_gc();
    INT have = dlb_compound_pending(1);
    dlb_compound_reset();
    { char m[200];
      snprintf(m, sizeof m, "[nosw] dlb: %d gc rounds, %d verified enqueued, %d kept\r\n",
               (int)loops, (int)nl_hrun, (int)have);
      pp_out(m); }
    pp_check(have == NL_HI, "N14", "verified dlb traces lost (of 50)", NL_HI - have);
    pp_check(nl_during > 0, "N15", "no starvation: enqueues while the runner was busy", nl_during);
}

/* N16-N17: the dkva KV cache (pk_nosw.h regions in dkva.c). As dkva_task
 * answers a peer's query, the runner computes the partial attention over the
 * cache over and over; as dtr does after each forward, a pri-19 task wakes
 * every 2 ms and rewrites all 8 entries with set A or set B, 60 times (one
 * wake = all 8, so between wakes the cache is all A or all B). Every partial
 * must be exactly A's or exactly B's. The cache is saved and put back.
 *   N16  no partial that is neither A's nor B's (mixed K/V)
 *   N17  no starvation: rewrites landed while the runner was busy
 * -DPK_NOSW_OFF must turn N16 red. */
#define NK_HI 60
IMPORT void dkva_probe_partial(const DKVA_Q_PKT *q, DKVA_RESP_PKT *r);
IMPORT void dkva_probe_kv(INT restore);
static float nk_k[2][DKVA_NH][DKVA_SEQ][DKVA_DH], nk_v[2][DKVA_NH][DKVA_SEQ][DKVA_DH];
static DKVA_Q_PKT nk_q;
static DKVA_RESP_PKT nk_ra, nk_rb, nk_r;
static volatile INT nk_hrun, nk_during, nk_busy;
static void nk_fill(INT s)
{
    for (INT e = 0; e < DKVA_CACHE_SIZE; e++) dkva_cache_update(nk_k[s], nk_v[s]);
}
static void t_nk_high(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    for (INT n = 0; n < NK_HI; n++) {
        tk_dly_tsk(2);
        if (nk_busy) nk_during++;
        nk_fill((n + 1) & 1);
        nk_hrun++;
    }
    tk_ext_tsk();
}
static void ns_dkva(void)
{
    for (INT h = 0; h < DKVA_NH; h++)
        for (INT t = 0; t < DKVA_SEQ; t++)
            for (INT d = 0; d < DKVA_DH; d++) {
                nk_k[0][h][t][d] = 0.25f + 0.01f * (float)d;
                nk_v[0][h][t][d] = 1.0f + (float)t;
                nk_k[1][h][t][d] = -0.5f + 0.02f * (float)h;
                nk_v[1][h][t][d] = -2.0f - (float)d;
            }
    memset(&nk_q, 0, sizeof nk_q);
    for (INT t = 0; t < DKVA_SEQ; t++)
        for (INT h = 0; h < DKVA_NH; h++)
            for (INT d = 0; d < DKVA_DH; d++) nk_q.Q[t][h][d] = 0.3f + 0.1f * (float)t;
    dkva_probe_kv(0);
    nk_fill(1); dkva_probe_partial(&nk_q, &nk_rb);
    nk_fill(0); dkva_probe_partial(&nk_q, &nk_ra);
    INT calls = 0, mixed = 0;
    nk_hrun = nk_during = 0;
    ID t = pp_mk(t_nk_high, 19);
    if (t > 0) tk_sta_tsk(t, 0);
    nk_busy = 1;
    while (nk_hrun < NK_HI && calls < 20000000) {
        dkva_probe_partial(&nk_q, &nk_r);
        calls++;
        if (memcmp(&nk_r, &nk_ra, sizeof nk_r) && memcmp(&nk_r, &nk_rb, sizeof nk_r)) mixed++;
    }
    nk_busy = 0;
    for (INT n = 0; n < 200 && nk_hrun < NK_HI; n++) tk_dly_tsk(10);
    if (t > 0) { tk_ter_tsk(t); tk_del_tsk(t); }
    dkva_probe_kv(1);
    INT differ = memcmp(&nk_ra, &nk_rb, sizeof nk_ra) != 0;
    { char m[200];
      snprintf(m, sizeof m, "[nosw] dkva: %d partials, %d cache rewrites, A and B differ: %s\r\n",
               (int)calls, (int)nk_hrun, differ ? "yes" : "NO");
      pp_out(m); }
    pp_check(mixed == 0 && differ, "N16", "dkva partials that were neither A's nor B's", mixed);
    pp_check(nk_during > 0, "N17", "no starvation: rewrites while the runner was busy", nk_during);
}

/* N18-N19: the pool inside a no-switch region. The runner enters a region,
 * runs a pool job (~ms) and checks no other task ran before it leaves; a
 * pri-19 task wakes every 2 ms, 30 times. pk_pool_left (preempt.c) used to
 * dispatch at the pool's end regardless of the region.
 *   N18  no switch inside the region at the pool's end
 *   N19  no starvation: the high task ran between the runner's regions
 * -DPK_POOL_LEAVE_IGNORES_NOSW (the old pk_pool_left) must turn N18 red. */
#define NQ_HI 30
static volatile INT nq_hrun;
static void t_nq_high(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    for (INT n = 0; n < NQ_HI; n++) { tk_dly_tsk(2); nq_hrun++; }
    tk_ext_tsk();
}
static void ns_poolregion(void)
{
    INT rounds = 0, inside = 0;
    nq_hrun = 0;
    ID t = pp_mk(t_nq_high, 19);
    if (t > 0) tk_sta_tsk(t, 0);
    while (nq_hrun < NQ_HI && rounds < 100000) {
        struct pp_ctx c = { pp_L, 3000 + (UW)rounds, 2000 };
        PK_NOSW_ENTER();
        INT h0 = nq_hrun;
        pk_parallel_rows(PP_ROWS, pp_body, &c);
        if (nq_hrun != h0) inside++;
        PK_NOSW_LEAVE();
        rounds++;
    }
    for (INT n = 0; n < 200 && nq_hrun < NQ_HI; n++) tk_dly_tsk(10);
    if (t > 0) { tk_ter_tsk(t); tk_del_tsk(t); }
    { char m[200];
      snprintf(m, sizeof m, "[nosw] pool in a region: %d rounds, %d high wakes\r\n",
               (int)rounds, (int)nq_hrun);
      pp_out(m); }
    pp_check(inside == 0, "N18", "rounds where another task ran inside the region", inside);
    pp_check(nq_hrun == NQ_HI, "N19", "no starvation: high wakes finished (of 30)", nq_hrun);
}

/* N20-N22: net_lan_send's static packet buffer and nonce counter (the
 * region in net_lan.c lan_send). The runner sends 1000-byte frames of 0xAA
 * over and over; a pri-19 task wakes every 2 ms and sends a frame of 0xBB,
 * 60 times. The packets go into private buffers instead of the socket
 * (net_lan_probe_send), on the v2 wire with a zero key.
 *   N20  packets that are not the sender's own (payload or MAC)
 *   N21  nonces lost: next_nonce advanced less than the number of sends
 *   N22  no starvation: sends landed while the runner was busy
 * -DPK_NOSW_OFF must turn N20 red.
 * The zero key is the live key: while the LAN is up (PKERNEL_LAN=1) the probe
 * refuses, N20-N22 are SKIPPED (the summary says so, not ALL PASS) and
 *   N23  live LAN: the probe was refused and the key stayed as it was
 * -DPK_NETLAN_PROBE_NO_GUARD must turn N23 red (run_nosw.sh netlive arms). */
#define NW_HI  60
#define NW_LEN 1000
IMPORT int net_lan_probe(int on, unsigned long long *nonce);
IMPORT int net_lan_probe_send(const void *frame, int len, unsigned char *out);
IMPORT int net_lan_live(void);
IMPORT int net_lan_key_copy(unsigned char *out, int cap);
static volatile INT nw_hrun, nw_hbad, nw_during, nw_busy;
static void t_nw_high(INT stacd, void *exinf)
{
    static UB f[NW_LEN], o[NW_LEN + 64];
    (void)stacd; (void)exinf;
    memset(f, 0xBB, sizeof f);
    for (INT n = 0; n < NW_HI; n++) {
        tk_dly_tsk(2);
        if (nw_busy) nw_during++;
        if (!net_lan_probe_send(f, NW_LEN, o)) nw_hbad++;
        nw_hrun++;
    }
    tk_ext_tsk();
}
/* N24-N25: net_lan's peer table (learn_peer publishes the count, then copies
 * the address; lan_send walks the table from any task). The runner fills the
 * emptied table with 64 peers over and over; a pri-19 task wakes every 2 ms
 * and walks it as lan_send does, 60 times.
 *   N24  slots walked while still empty (counted but not yet copied)
 *   N25  no starvation: walks while the runner was filling
 * With -DNETLAN_FAULT_SLOW_LEARN=<spins> (the gap widened), -DPK_NOSW_OFF must
 * turn N24 red (run_nosw.sh slowpeer / slowpeeroff). */
#define LP_HI 60
IMPORT int  net_lan_probe_peers_reset(void);
IMPORT void net_lan_probe_learn(unsigned i);
IMPORT int  net_lan_probe_peers_bad(void);
static volatile INT lp_hrun, lp_bad, lp_during, lp_busy;
static void t_lp_high(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    for (INT n = 0; n < LP_HI; n++) {
        tk_dly_tsk(2);
        if (lp_busy) lp_during++;
        lp_bad += net_lan_probe_peers_bad();
        lp_hrun++;
    }
    tk_ext_tsk();
}
static void ns_netpeer(void)
{
    INT rounds = 0;
    if (net_lan_probe_peers_reset() < 0) { pp_out("[nosw] FAIL setup: peer table probe refused\r\n"); pp_fail++; return; }
    lp_hrun = lp_bad = lp_during = 0;
    ID t = pp_mk(t_lp_high, 19);
    if (t > 0) tk_sta_tsk(t, 0);
    lp_busy = 1;
    while (lp_hrun < LP_HI && rounds < 1000000) {
        for (unsigned i = 0; i < 64; i++) net_lan_probe_learn(i);
        net_lan_probe_peers_reset();
        rounds++;
    }
    lp_busy = 0;
    for (INT n = 0; n < 200 && lp_hrun < LP_HI; n++) tk_dly_tsk(10);
    if (t > 0) { tk_ter_tsk(t); tk_del_tsk(t); }
    net_lan_probe_peers_reset();
    { char m[160];
      snprintf(m, sizeof m, "[nosw] net_lan peers: %d fills of 64, %d walks\r\n", (int)rounds, (int)lp_hrun);
      pp_out(m); }
    pp_check(lp_bad == 0, "N24", "net_lan peer slots walked while still empty", lp_bad);
    pp_check(lp_during > 0, "N25", "no starvation: peer walks while the runner was filling", lp_during);
}
static void ns_netlan(void)
{
    static UB f[NW_LEN], o[NW_LEN + 64];
    INT sends = 0, bad = 0;
    unsigned long long n0, n1;
    if (net_lan_live()) {
        unsigned char k0[64], k1[64];
        int l0 = net_lan_key_copy(k0, sizeof k0);
        int r = net_lan_probe(1, &n0);
        int l1 = net_lan_key_copy(k1, sizeof k1);
        int diff = 0;
        if (r == 0) net_lan_probe(0, &n1);
        for (int i = 0; l0 > 0 && i < l0; i++) if (k0[i] != k1[i]) diff++;
        { char m[160];
          snprintf(m, sizeof m, "[nosw] net_lan is live: probe %s, key bytes changed %d of %d\r\n",
                   r < 0 ? "refused" : "ran", diff, l0);
          pp_out(m); }
        pp_check(r < 0 && l0 > 0 && l1 == l0 && memcmp(k0, k1, (size_t)l0) == 0, "N23",
                 "live LAN: the probe left the key alone (key bytes changed during it)", (long)diff);
        pp_out("[nosw] SKIP N20 N21 N22  net_lan is live (the probe needs its own zero key)\r\n");
        pp_out("[nosw] SKIP N24 N25  net_lan is live (the probe empties the peer table)\r\n");
        pp_skip += 5;
        return;
    }
    memset(f, 0xAA, sizeof f);
    net_lan_probe(1, &n0);
    nw_hrun = nw_hbad = nw_during = 0;
    ID t = pp_mk(t_nw_high, 19);
    if (t > 0) tk_sta_tsk(t, 0);
    nw_busy = 1;
    while (nw_hrun < NW_HI && sends < 20000000) {
        if (!net_lan_probe_send(f, NW_LEN, o)) bad++;
        sends++;
    }
    nw_busy = 0;
    for (INT n = 0; n < 200 && nw_hrun < NW_HI; n++) tk_dly_tsk(10);
    if (t > 0) { tk_ter_tsk(t); tk_del_tsk(t); }
    net_lan_probe(0, &n1);
    long long lost = (long long)(sends + nw_hrun) - (long long)(n1 - n0);
    { char m[200];
      snprintf(m, sizeof m, "[nosw] net_lan: %d runner sends, %d high sends, nonces %llu\r\n",
               (int)sends, (int)nw_hrun, n1 - n0);
      pp_out(m); }
    pp_check(bad + nw_hbad == 0, "N20", "net_lan packets that were not the sender's own", bad + nw_hbad);
    pp_check(lost == 0, "N21", "net_lan nonces lost (sends - nonces minted)", (INT)lost);
    pp_check(nw_during > 0, "N22", "no starvation: net_lan sends while the runner was busy", nw_during);
    ns_netpeer();
}

/* N26-N27: drpc's pending table (drpc_claim: search, seq, fill, in_use). The
 * runner claims and releases a slot over and over; a pri-19 task wakes every
 * 2 ms, claims a slot, holds it over a 1 ms sleep and checks it still holds
 * its own claim, 60 times. The runner checks its claim too.
 *   N26  claims found overwritten by another claim (the same slot twice)
 *   N27  no starvation: high claims while the runner was claiming
 * With -DDRPC_FAULT_SLOW_CLAIM=<spins> (the gap widened), -DPK_NOSW_OFF must
 * turn N26 red (run_nosw.sh slowclaim / slowclaimoff). */
#define DC_HI 60
IMPORT INT  drpc_probe_claim(UH *seq);
IMPORT BOOL drpc_probe_mine(INT slot, UH seq);
IMPORT void drpc_probe_release(INT slot);
static volatile INT dc_hrun, dc_bad, dc_during, dc_busy, dc_full;
static void t_dc_high(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    for (INT n = 0; n < DC_HI; n++) {
        UH q = 0;
        tk_dly_tsk(2);
        if (dc_busy) dc_during++;
        INT sl = drpc_probe_claim(&q);
        if (sl < 0) { dc_full++; dc_hrun++; continue; }
        tk_dly_tsk(1);
        if (!drpc_probe_mine(sl, q)) dc_bad++;
        drpc_probe_release(sl);
        dc_hrun++;
    }
    tk_ext_tsk();
}
static void ns_drpc(void)
{
    INT claims = 0, bad = 0;
    dc_hrun = dc_bad = dc_during = dc_full = 0;
    ID t = pp_mk(t_dc_high, 19);
    if (t > 0) tk_sta_tsk(t, 0);
    dc_busy = 1;
    while (dc_hrun < DC_HI && claims < 50000000) {
        UH q = 0;
        INT sl = drpc_probe_claim(&q);
        if (sl < 0) { dc_full++; continue; }
        if (!drpc_probe_mine(sl, q)) bad++;
        drpc_probe_release(sl);
        claims++;
    }
    dc_busy = 0;
    for (INT n = 0; n < 200 && dc_hrun < DC_HI; n++) tk_dly_tsk(10);
    if (t > 0) { tk_ter_tsk(t); tk_del_tsk(t); }
    { char m[160];
      snprintf(m, sizeof m, "[nosw] drpc: %d runner claims, %d high claims, table full %d times\r\n",
               (int)claims, (int)dc_hrun, (int)dc_full);
      pp_out(m); }
    pp_check(bad + dc_bad == 0, "N26", "drpc claims overwritten by another claim", bad + dc_bad);
    pp_check(dc_during > 0, "N27", "no starvation: high claims while the runner was claiming", dc_during);
}

/* N28-N30: net_relay_send's static packet buffer and the relay's nonce
 * counter (relay_bcast in arch/linux/<arch>/net_relay.c; ROADMAP 0-10, hosted
 * aarch64 had no region there). The same shape as N20-N22: the runner sends
 * 1000-byte frames of 0xAA, a pri-19 task wakes every 2 ms and sends 0xBB,
 * 60 times, into private buffers on the v2 wire with a zero key.
 *   N28  packets that are not the sender's own (payload or MAC)
 *   N29  nonces lost: next_nonce advanced less than the number of sends
 *   N30  no starvation: sends landed while the runner was busy
 * -DPK_NOSW_OFF must turn N28 red. While the relay is up the probe refuses
 * and N28-N30 are SKIPPED. `tkconf nr` runs only this part. */
IMPORT int net_relay_nosw_probe(int on, unsigned long long *nonce);
IMPORT int net_relay_nosw_send(const void *frame, int len, unsigned char *out);
IMPORT int net_relay_live(void);
static volatile INT nr_hrun, nr_hbad, nr_during, nr_busy;
static void t_nr_high(INT stacd, void *exinf)
{
    static UB f[NW_LEN], o[NW_LEN + 64];
    (void)stacd; (void)exinf;
    memset(f, 0xBB, sizeof f);
    for (INT n = 0; n < NW_HI; n++) {
        tk_dly_tsk(2);
        if (nr_busy) nr_during++;
        if (!net_relay_nosw_send(f, NW_LEN, o)) nr_hbad++;
        nr_hrun++;
    }
    tk_ext_tsk();
}
static void ns_netrelay(void)
{
    static UB f[NW_LEN], o[NW_LEN + 64];
    INT sends = 0, bad = 0;
    unsigned long long n0, n1;
    if (net_relay_live() || net_relay_nosw_probe(1, &n0) < 0) {
        pp_out("[nosw] SKIP N28 N29 N30  the relay is live (the probe needs its own zero key)\r\n");
        pp_skip += 3;
        return;
    }
    memset(f, 0xAA, sizeof f);
    nr_hrun = nr_hbad = nr_during = 0;
    ID t = pp_mk(t_nr_high, 19);
    if (t > 0) tk_sta_tsk(t, 0);
    nr_busy = 1;
    while (nr_hrun < NW_HI && sends < 20000000) {
        if (!net_relay_nosw_send(f, NW_LEN, o)) bad++;
        sends++;
    }
    nr_busy = 0;
    for (INT n = 0; n < 200 && nr_hrun < NW_HI; n++) tk_dly_tsk(10);
    if (t > 0) { tk_ter_tsk(t); tk_del_tsk(t); }
    net_relay_nosw_probe(0, &n1);
    long long lost = (long long)(sends + nr_hrun) - (long long)(n1 - n0);
    { char m[200];
      snprintf(m, sizeof m, "[nosw] net_relay: %d runner sends, %d high sends, nonces %llu\r\n",
               (int)sends, (int)nr_hrun, n1 - n0);
      pp_out(m); }
    pp_check(bad + nr_hbad == 0, "N28", "net_relay packets that were not the sender's own", bad + nr_hbad);
    pp_check(lost == 0, "N29", "net_relay nonces lost (sends - nonces minted)", (INT)lost);
    pp_check(nr_during > 0, "N30", "no starvation: net_relay sends while the runner was busy", nr_during);
}

static void t_nr_runner(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    ns_netrelay();
    pp_done = 1;
    tk_wup_tsk(pp_caller);
    tk_ext_tsk();
}

static void t_ns_runner(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    UB b[NS_LEN];
    INT torn = 0, reads = 0;
    ns_pubs = ns_during = 0;
    W h = kdds_open_poll_scoped("tkc/nosw", KDDS_QOS_LATEST_ONLY, KDDS_SCOPE_GLOBAL);
    if (h < 0) { pp_out("[nosw] FAIL setup: kdds_open\r\n"); pp_fail++; goto done; }
    for (INT i = 0; i < NS_LEN; i++) b[i] = 1;
    kdds_pub(h, b, NS_LEN);
    ID t = pp_mk(t_ns_high, 19);
    if (t > 0) tk_sta_tsk(t, 0);
    ns_reading = 1;
    for (INT r = 0; r < 4000000 && ns_pubs < NS_PUBS; r++) {
        if (kdds_sub(h, b, NS_LEN, 0) != NS_LEN) continue;
        reads++;
        for (INT i = 1; i < NS_LEN; i++) if (b[i] != b[0]) { torn++; break; }
    }
    ns_reading = 0;
    for (INT n = 0; n < 100 && ns_pubs < NS_PUBS; n++) tk_dly_tsk(10);
    { char m[200];
      snprintf(m, sizeof m, "[nosw] %d reads, %d publishes\r\n", (int)reads, (int)ns_pubs);
      pp_out(m); }
    pp_check(torn == 0, "N1", "torn reads of the topic slot", torn);
    pp_check(ns_pubs == NS_PUBS, "N2", "high task: publishes finished (of 60)", ns_pubs);
    pp_check(ns_during > 0, "N3", "no starvation: publishes while the runner was reading", ns_during);
    if (t > 0) { tk_ter_tsk(t); tk_del_tsk(t); }
    kdds_close(h);
    ns_pfs();
    ns_student();
    ns_ark();
    ns_dtr();
    ns_dlb();
    ns_dkva();
    ns_poolregion();
    ns_netlan();
    ns_netrelay();
    ns_drpc();
done:
    pp_done = 1;
    tk_wup_tsk(pp_caller);
    tk_ext_tsk();
}

INT nosw_run(void (*out)(const char *))
{
    pp_out = out; pp_fail = 0; pp_skip = 0; pp_done = 0; pp_tag = "[nosw]";
    out("[nosw] a kdds topic slot under task switches (runner pri 20, publisher pri 19)\r\n");
    pp_caller = tk_get_tid();
    ID run = pp_mk(t_ns_runner, 20);
    if (run <= 0) { out("[nosw] FAIL setup: no runner task\r\n"); return 1; }
    tk_can_wup(TSK_SELF);
    tk_sta_tsk(run, 0);
    /* 900 s, not 120: with the regions on, the high task of the student part
     * runs once per ST_TRAIN_STEP (the step is one region), and under
     * qemu-aarch64-static a step of the M baby takes ~15 s. At 120 s the
     * runner was in that phase, depth 0, ready, 6-7 steps done (2/2); with
     * 1500 s the whole run was ALL PASS in 370 s (2026-10-07, /build/a64d-e).
     * Native x86_64 finishes far inside either limit. */
    while (!pp_done && tk_slp_tsk(900000) == E_OK) { }
    if (!pp_done) { out("[nosw] FAIL setup: the runner did not finish within 900 s\r\n"); pp_fail++; tk_ter_tsk(run); }
    tk_del_tsk(run);
    out(pp_fail ? "[nosw] RED\r\n" :
        pp_skip ? "[nosw] PASS, BUT SOME CHECKS SKIPPED (see the SKIP lines: net_lan or the relay is live)\r\n" :
                  "[nosw] ALL PASS\r\n");
    return pp_fail;
}

/* `tkconf nr`: only N28-N30 (the relay sender), short enough for the hosted
 * aarch64 CI step where the whole `tkconf nosw` (~10 min under qemu-user) is
 * not run. */
INT nosw_relay_run(void (*out)(const char *))
{
    pp_out = out; pp_fail = 0; pp_skip = 0; pp_done = 0; pp_tag = "[nosw]";
    out("[nosw] the relay sender under task switches (runner pri 20, sender pri 19)\r\n");
    pp_caller = tk_get_tid();
    ID run = pp_mk(t_nr_runner, 20);
    if (run <= 0) { out("[nosw] FAIL setup: no runner task\r\n"); return 1; }
    tk_can_wup(TSK_SELF);
    tk_sta_tsk(run, 0);
    while (!pp_done && tk_slp_tsk(300000) == E_OK) { }
    if (!pp_done) { out("[nosw] FAIL setup: the runner did not finish within 300 s\r\n"); pp_fail++; tk_ter_tsk(run); }
    tk_del_tsk(run);
    out(pp_fail ? "[nosw] RED\r\n" :
        pp_skip ? "[nosw] PASS, BUT N28-N30 SKIPPED (the relay is live)\r\n" :
                  "[nosw] ALL PASS\r\n");
    return pp_fail;
}

/* `tkconf fp` (FP-PRESERVE, audit-26): the exit dispatch must give a task back
 * its FP/SIMD registers. The runner (pri 20) steps 8 doubles held in registers
 * through chunks of a bounded linear map while a pri-19 task wakes every 2 ms
 * and does FP work of its own, 60 times. Then the same number of chunks again
 * with dispatch disabled. The two results must be equal bit for bit.
 *   F1  doubles unlike the run without switches (of 8)
 *   F2  precondition: high FP rounds that landed inside the runner's chunks
 * -DPK_PREEMPT_DROP_FP (preempt.c: on x86_64 the trampoline zeroes xmm0-15
 * instead of restoring them; on aarch64 the frame copy's fpsimd record loses
 * v0-v31) must turn F1 red.
 * x86_64 only (test-holes H11: the compiler keeps fp_chunk's doubles in the low
 * xmm, so losing xmm12-15 or MXCSR stayed green): a second round where the runner
 * holds 4 doubles in xmm12-15 and round-toward-zero in MXCSR inside an asm loop
 * while the high task fills xmm12-15 with ones and sleeps under round-down.
 *   F3  xmm12-15 unlike what the runner put there (of 4)
 *   F4  MXCSR unlike the runner's 0x7f80 (got the value read back)
 *   F5  precondition: high rounds while the runner held them
 * -DPK_PREEMPT_DROP_FP_HI turns F3 red, -DPK_PREEMPT_DROP_MXCSR F4. Not looked
 * at: x87, ymm upper halves, and on aarch64 a partial loss (v8-v31). */
#define FP_HI    60
#define FP_HI2   30
#define FP_ITERS 20000
static volatile INT fp_hrun, fp_during, fp_busy, fp_rounds;
static volatile int fp_hdone;
static volatile double fp_sink;

/* F6, F7 (both arches): the voluntary switch (knl_dispatch_entry, called like a
 * function) must keep the FP state the ABI calls callee-saved: on x86_64 the
 * control bits of MXCSR and the x87 control word, on aarch64 d8-d15 and FPCR.
 * fp_hold_call(fn, in, out) loads `in` into them, calls fn (which starts a higher
 * task that sets its own and exits, so the runner is switched out and back
 * inside), stores them to `out` and puts its caller's back.
 * -DPK_DISPATCH_NO_FPCTL (dispatch.S without the save) turns F6 red. */
void fp_hold_call(void (*fn)(void), const void *in, void *out);
#if defined(__x86_64__)
__asm__(
    "    .text\n"
    "    .globl fp_hold_call\n"
    "    .type  fp_hold_call, @function\n"
    "fp_hold_call:\n"                       /* rdi fn, rsi in {u32 mxcsr, u16 cw}, rdx out */
    "    pushq %r12\n"
    "    subq  $16, %rsp\n"
    "    stmxcsr 0(%rsp)\n"
    "    fnstcw  4(%rsp)\n"
    "    movq  %rdx, %r12\n"
    "    ldmxcsr 0(%rsi)\n"
    "    fldcw   4(%rsi)\n"
    "    call  *%rdi\n"
    "    stmxcsr 0(%r12)\n"
    "    fnstcw  4(%r12)\n"
    "    ldmxcsr 0(%rsp)\n"
    "    fldcw   4(%rsp)\n"
    "    addq  $16, %rsp\n"
    "    popq  %r12\n"
    "    ret\n"
    "    .size fp_hold_call, . - fp_hold_call\n");
#elif defined(__aarch64__)
__asm__(
    "    .text\n"
    "    .globl fp_hold_call\n"
    "    .type  fp_hold_call, %function\n"
    "fp_hold_call:\n"                       /* x0 fn, x1 in {d8..d15, u64 fpcr}, x2 out */
    "    stp  x29, x30, [sp, #-112]!\n"
    "    mov  x29, sp\n"
    "    stp  d8,  d9,  [sp, #16]\n"
    "    stp  d10, d11, [sp, #32]\n"
    "    stp  d12, d13, [sp, #48]\n"
    "    stp  d14, d15, [sp, #64]\n"
    "    mrs  x3, fpcr\n"
    "    stp  x3, x19,  [sp, #80]\n"
    "    mov  x19, x2\n"
    "    ldp  d8,  d9,  [x1]\n"
    "    ldp  d10, d11, [x1, #16]\n"
    "    ldp  d12, d13, [x1, #32]\n"
    "    ldp  d14, d15, [x1, #48]\n"
    "    ldr  x3, [x1, #64]\n"
    "    msr  fpcr, x3\n"
    "    blr  x0\n"
    "    stp  d8,  d9,  [x19]\n"
    "    stp  d10, d11, [x19, #16]\n"
    "    stp  d12, d13, [x19, #32]\n"
    "    stp  d14, d15, [x19, #48]\n"
    "    mrs  x3, fpcr\n"
    "    str  x3, [x19, #64]\n"
    "    ldp  x3, x19,  [sp, #80]\n"
    "    msr  fpcr, x3\n"
    "    ldp  d8,  d9,  [sp, #16]\n"
    "    ldp  d10, d11, [sp, #32]\n"
    "    ldp  d12, d13, [sp, #48]\n"
    "    ldp  d14, d15, [sp, #64]\n"
    "    ldp  x29, x30, [sp], #112\n"
    "    ret\n"
    "    .size fp_hold_call, . - fp_hold_call\n");
#endif
static volatile ID fpctl_tid;
static volatile int fpctl_ran;
static void t_fp_ctl(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
#if defined(__x86_64__)
    static const unsigned mx_rd = 0x3f80;            /* round down */
    static const unsigned short cw_rd = 0x077f;      /* x87: round down */
    __asm__ volatile("ldmxcsr %0\n\tfldcw %1" : : "m"(mx_rd), "m"(cw_rd));
#elif defined(__aarch64__)
    __asm__ volatile("fmov d8, #-1.0\n\tfmov d9, #-1.0\n\tfmov d10, #-1.0\n\tfmov d11, #-1.0\n\t"
                     "fmov d12, #-1.0\n\tfmov d13, #-1.0\n\tfmov d14, #-1.0\n\tfmov d15, #-1.0\n\t"
                     "mov x9, #0x800000\n\tmsr fpcr, x9"   /* RMode = toward minus infinity */
                     : : : "x9", "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15");
#endif
    fpctl_ran = 1;
    tk_ext_tsk();
}
static void fpctl_kick(void) { tk_sta_tsk(fpctl_tid, 0); }
static void fp_chunk(double s[8])
{
    double a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (INT i = 0; i < FP_ITERS; i++) {
        double t = a;
        a = a * 0.9999999 + b * 1e-7;  b = b * 0.9999999 + c * 1e-7;
        c = c * 0.9999999 + d * 1e-7;  d = d * 0.9999999 + e * 1e-7;
        e = e * 0.9999999 + f * 1e-7;  f = f * 0.9999999 + g * 1e-7;
        g = g * 0.9999999 + h * 1e-7;  h = h * 0.9999999 + t * 1e-7;
    }
    s[0] = a; s[1] = b; s[2] = c; s[3] = d; s[4] = e; s[5] = f; s[6] = g; s[7] = h;
}
static void t_fp_high(INT stacd, void *exinf)
{
    double x[8] = { -3.5, 2.25, 9.0, -0.125, 7.75, -6.0, 0.5, 4.0 };
    (void)stacd; (void)exinf;
    for (INT n = 0; n < fp_rounds; n++) {
#if defined(__x86_64__)
        /* H11: sleep under round-down (0x3f80) with xmm12-15 all ones, so a
         * switch back that does not restore the runner's own shows */
        static const unsigned mx_rd = 0x3f80, mx_def = 0x1f80;
        __asm__ volatile("pcmpeqd %%xmm12, %%xmm12\n\tpcmpeqd %%xmm13, %%xmm13\n\t"
                         "pcmpeqd %%xmm14, %%xmm14\n\tpcmpeqd %%xmm15, %%xmm15\n\t"
                         "ldmxcsr %0" : : "m"(mx_rd) : "xmm12", "xmm13", "xmm14", "xmm15");
        tk_dly_tsk(2);
        __asm__ volatile("ldmxcsr %0" : : "m"(mx_def));
#else
        tk_dly_tsk(2);
#endif
        if (fp_busy) fp_during++;
        fp_chunk(x);
        fp_hrun++;
    }
    fp_sink = x[0];
    fp_hdone = 1;
    tk_ext_tsk();
}
static void t_fp_runner(INT stacd, void *exinf)
{
    static const double s0[8] = { 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8 };
    double s[8], r[8];
    INT chunks = 0, diff = 0;
    (void)stacd; (void)exinf;
    memcpy(s, s0, sizeof s);
    fp_hrun = fp_during = 0; fp_rounds = FP_HI; fp_hdone = 0;
    ID t = pp_mk(t_fp_high, 19);
    if (t > 0) tk_sta_tsk(t, 0);
    fp_busy = 1;
    while (fp_hrun < FP_HI && chunks < 1000000) { fp_chunk(s); chunks++; }
    fp_busy = 0;
    for (INT n = 0; n < 200 && fp_hrun < FP_HI; n++) tk_dly_tsk(10);
    if (t > 0) { tk_ter_tsk(t); tk_del_tsk(t); }
    memcpy(r, s0, sizeof r);
    tk_dis_dsp();
    for (INT k = 0; k < chunks; k++) fp_chunk(r);
    tk_ena_dsp();
    for (INT i = 0; i < 8; i++) if (memcmp(&s[i], &r[i], sizeof s[i])) diff++;
    { char m[200];
      snprintf(m, sizeof m, "[fp] %d chunks of %d steps, high rounds %d, s[0] %.17g / %.17g\r\n",
               (int)chunks, FP_ITERS, (int)fp_hrun, s[0], r[0]);
      pp_out(m); }
    pp_check(diff == 0, "F1", "doubles unlike the run without switches (of 8)", diff);
    pp_check(fp_during > 0, "F2", "precondition: high FP rounds inside the runner's chunks", fp_during);
#if defined(__x86_64__)
    {   /* H11: xmm12-15 and MXCSR held in asm across the high task's rounds */
        static const double hin[4] = { 1.25, -2.5, 3.75, -5.0 };
        static const unsigned mx_set = 0x7f80;      /* default masks, round toward zero */
        double hout[4] = { 0, 0, 0, 0 };
        unsigned mx_saved = 0, mx_got = 0;
        unsigned long spin = 500000000UL;   /* bound (~2 s to ~1 min of pause): the high task may never finish */
        INT hdiff = 0;
        fp_hrun = fp_during = 0; fp_rounds = FP_HI2; fp_hdone = 0;
        ID t2 = pp_mk(t_fp_high, 19);
        if (t2 > 0) tk_sta_tsk(t2, 0);
        fp_busy = 1;
        __asm__ volatile(
            "stmxcsr %[sv]\n\t"
            "ldmxcsr %[set]\n\t"
            "movsd  0(%[in]), %%xmm12\n\t"
            "movsd  8(%[in]), %%xmm13\n\t"
            "movsd 16(%[in]), %%xmm14\n\t"
            "movsd 24(%[in]), %%xmm15\n\t"
            "1:\n\t"
            "pause\n\t"
            "cmpl  $0, %[done]\n\t"
            "jne   2f\n\t"
            "dec   %[spin]\n\t"
            "jnz   1b\n\t"
            "2:\n\t"
            "movsd %%xmm12,  0(%[out])\n\t"
            "movsd %%xmm13,  8(%[out])\n\t"
            "movsd %%xmm14, 16(%[out])\n\t"
            "movsd %%xmm15, 24(%[out])\n\t"
            "stmxcsr %[got]\n\t"
            "ldmxcsr %[sv]\n\t"
            : [sv] "+m"(mx_saved), [got] "=m"(mx_got), [spin] "+r"(spin)
            : [set] "m"(mx_set), [in] "r"(hin), [out] "r"(hout), [done] "m"(fp_hdone)
            : "xmm12", "xmm13", "xmm14", "xmm15", "memory", "cc");
        fp_busy = 0;
        for (INT n = 0; n < 200 && !fp_hdone; n++) tk_dly_tsk(10);
        if (t2 > 0) { tk_ter_tsk(t2); tk_del_tsk(t2); }
        for (INT i = 0; i < 4; i++) if (memcmp(&hout[i], &hin[i], sizeof hin[i])) hdiff++;
        { char m[200];
          snprintf(m, sizeof m, "[fp] xmm12-15 held across %d high rounds: %g %g %g %g, MXCSR %#x%s\r\n",
                   (int)fp_hrun, hout[0], hout[1], hout[2], hout[3], mx_got,
                   spin ? "" : " (spin bound reached)");
          pp_out(m); }
        pp_check(hdiff == 0, "F3", "xmm12-15 unlike what the runner put there (of 4)", hdiff);
        pp_check(mx_got == mx_set, "F4", "MXCSR unlike the runner's 0x7f80 (got)", (long)mx_got);
        pp_check(fp_during > 0, "F5", "precondition: high rounds while the runner held xmm12-15", fp_during);
    }
#endif
#if defined(__x86_64__) || defined(__aarch64__)
    {   /* F6, F7: the callee-saved FP state across a voluntary switch */
        INT bad = 0;
        char m[200];
        fpctl_ran = 0;
        fpctl_tid = pp_mk(t_fp_ctl, 19);
#if defined(__x86_64__)
        static const struct { unsigned mx; unsigned short cw; } in = { 0x7f80, 0x0f7f };  /* toward zero */
        struct { unsigned mx; unsigned short cw; } out = { 0, 0 };
        if (fpctl_tid > 0) fp_hold_call(fpctl_kick, &in, &out);
        bad += (out.mx & ~0x3fu) != in.mx;   /* the status bits are caller-saved */
        bad += out.cw != in.cw;
        snprintf(m, sizeof m, "[fp] across a voluntary switch: MXCSR %#x (want %#x), x87 CW %#x (want %#x)\r\n",
                 out.mx, in.mx, out.cw, in.cw);
#else
        static const struct { double d[8]; unsigned long fpcr; } in =
            { { 1.5, -2.25, 3.125, -4.0, 5.5, -6.75, 7.0, -8.5 }, 0xC00000UL };            /* toward zero */
        struct { double d[8]; unsigned long fpcr; } out;
        memset(&out, 0, sizeof out);
        if (fpctl_tid > 0) fp_hold_call(fpctl_kick, &in, &out);
        for (INT i = 0; i < 8; i++) if (memcmp(&out.d[i], &in.d[i], sizeof in.d[i])) bad++;
        bad += (out.fpcr & 0xC00000UL) != in.fpcr;
        snprintf(m, sizeof m, "[fp] across a voluntary switch: d8 %g d15 %g (want 1.5 -8.5), FPCR RMode %#lx (want %#lx)\r\n",
                 out.d[0], out.d[7], out.fpcr & 0xC00000UL, in.fpcr);
#endif
        pp_out(m);
        if (fpctl_tid > 0) tk_del_tsk(fpctl_tid);
        pp_check(bad == 0, "F6", "callee-saved FP state unlike before a voluntary switch", bad);
        pp_check(fpctl_ran, "F7", "precondition: the higher task ran inside the call", fpctl_ran);
    }
#endif
    pp_done = 1;
    tk_wup_tsk(pp_caller);
    tk_ext_tsk();
}

INT fp_preserve_run(void (*out)(const char *))
{
    pp_out = out; pp_fail = 0; pp_done = 0; pp_tag = "[fp]";
    out("[fp] FP/SIMD registers across task switches (runner pri 20, high pri 19)\r\n");
    pp_caller = tk_get_tid();
    ID run = pp_mk(t_fp_runner, 20);
    if (run <= 0) { out("[fp] FAIL setup: no runner task\r\n"); return 1; }
    tk_can_wup(TSK_SELF);
    tk_sta_tsk(run, 0);
    while (!pp_done && tk_slp_tsk(120000) == E_OK) { }
    if (!pp_done) { out("[fp] FAIL setup: the runner did not finish within 120 s\r\n"); pp_fail++; tk_ter_tsk(run); }
    tk_del_tsk(run);
    out(pp_fail ? "[fp] RED\r\n" : "[fp] ALL PASS\r\n");
    return pp_fail;
}

INT pool_preempt_run(void (*out)(const char *))
{
    pp_out = out; pp_fail = 0; pp_done = 0; pp_tag = "[pool]";
    out("[pool] the shared matmul pool under task switches (runner pri 20, high pri 19)\r\n");
    pp_caller = tk_get_tid();
    ID run = pp_mk(t_pp_runner, 20);
    if (run <= 0) { out("[pool] FAIL setup: no runner task\r\n"); return 1; }
    tk_can_wup(TSK_SELF);
    tk_sta_tsk(run, 0);
    while (!pp_done && tk_slp_tsk(120000) == E_OK) { }
    if (!pp_done) { out("[pool] FAIL setup: the runner did not finish within 120 s\r\n"); pp_fail++; tk_ter_tsk(run); }
    tk_del_tsk(run);
    out(pp_fail ? "[pool] RED\r\n" : "[pool] ALL PASS\r\n");
    return pp_fail;
}
