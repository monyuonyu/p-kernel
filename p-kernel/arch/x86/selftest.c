/*
 *  selftest.c (x86)
 *  Kernel built-in self-test — runs at boot from the initial task,
 *  before any background tasks are started.
 *
 *  Twelve tests, ~100ms total:
 *   T1:  Semaphore signal/wait (no context switch)
 *   T2:  Timer fire via tk_dly_tsk (20ms sleep)
 *   T3:  Cyclic handler → tk_sig_sem  (Bug-1 regression: END_CRITICAL_SECTION
 *        must not dispatch from IRQ/task-independent context)
 *   T4:  Mutex lock/unlock across two tasks
 *   T5:  Config sanity (PAGING_MAX_TASKS vs CFN_MAX_TSKID)
 *   T6:  MLP forward determinism (same input → same class)
 *   T7:  MLP output bounds (class ∈ {0,1,2})
 *   T8:  FedLearn local_train returns E_OK
 *   T9:  FedLearn gradient — delta_b3 nonzero after finite-diff
 *   T10: Raft initial state (FOLLOWER, term=0, leader=0xFF)
 *   T11: Raft write guard (non-leader → E_RSFN)
 *   T12: Degrade initial level (FULL before first degrade_update)
 *   T13: Tensor alloc/write/read/free
 *   T14: Tensor zero (all bytes = 0 after tk_tensor_zero)
 *   T15: MemStore add/recent retrieval
 */

#include "kernel.h"
#include "paging.h"
#include "ai_kernel.h"
#include "raft.h"
#include "degrade.h"
#include "mem_store.h"

IMPORT void tm_putstring(UB *str);

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void st_puts(const char *s)
{
    tm_putstring((UB *)s);
}

static void st_puti(W n)
{
    char buf[12]; INT i = 11;
    buf[i] = '\0';
    BOOL neg = (n < 0);
    if (neg) n = -n;
    if (n == 0) { buf[--i] = '0'; }
    while (n > 0 && i > 0) { buf[--i] = (char)('0' + n % 10); n /= 10; }
    if (neg && i > 0) buf[--i] = '-';
    st_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* T3 state — must be file-scope so the cyclic handler can reach it   */
/* ------------------------------------------------------------------ */

static ID st_cyc_sem = 0;

static void st_cyc3_handler(VP exinf)
{
    (void)exinf;
    if (st_cyc_sem > 0)
        tk_sig_sem(st_cyc_sem, 1);
}

/* ------------------------------------------------------------------ */
/* T4 state — helper task for mutex test                              */
/* ------------------------------------------------------------------ */

static ID st_mtx_id      = 0;
static ID st_mtx_done    = 0;

static void st_mtx_helper(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    /* Will block here until the init task releases the mutex */
    tk_loc_mtx(st_mtx_id, TMO_FEVR);
    tk_unl_mtx(st_mtx_id);
    tk_sig_sem(st_mtx_done, 1);
    tk_ext_tsk();
}

/* ================================================================== */
/* Individual tests                                                    */
/* ================================================================== */

static int run_t1_sem(void)
{
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 4 };
    ID sid = tk_cre_sem(&cs);
    if (sid < E_OK) { st_puts("[FAIL] T1: tk_cre_sem er="); st_puti(sid); st_puts("\r\n"); return 0; }

    ER r;
    /* Signal once */
    r = tk_sig_sem(sid, 1);
    if (r != E_OK) { tk_del_sem(sid); st_puts("[FAIL] T1: sig er="); st_puti(r); st_puts("\r\n"); return 0; }

    /* Immediate poll — must succeed (count=1) */
    r = tk_wai_sem(sid, 1, TMO_POL);
    if (r != E_OK) { tk_del_sem(sid); st_puts("[FAIL] T1: poll1 er="); st_puti(r); st_puts("\r\n"); return 0; }

    /* Poll again — must timeout (count=0) */
    r = tk_wai_sem(sid, 1, TMO_POL);
    if (r != E_TMOUT) { tk_del_sem(sid); st_puts("[FAIL] T1: poll2 er="); st_puti(r); st_puts("\r\n"); return 0; }

    tk_del_sem(sid);
    st_puts("[PASS] T1: sem signal/wait\r\n");
    return 1;
}

static int run_t2_timer(void)
{
    SYSTIM t0, t1;
    tk_get_otm(&t0);
    tk_dly_tsk(20);    /* 20ms sleep — PIT must fire to wake us */
    tk_get_otm(&t1);

    W elapsed = (W)(t1.lo - t0.lo);
    if (elapsed < 20 || elapsed > 100) {
        st_puts("[FAIL] T2: timer elapsed="); st_puti(elapsed); st_puts("ms\r\n");
        return 0;
    }
    st_puts("[PASS] T2: timer 20ms dly\r\n");
    return 1;
}

static int run_t3_cyclic_bug1(void)
{
    /* Semaphore that the cyclic handler will signal */
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 4 };
    st_cyc_sem = tk_cre_sem(&cs);
    if (st_cyc_sem < E_OK) {
        st_puts("[FAIL] T3: sem er="); st_puti(st_cyc_sem); st_puts("\r\n");
        st_cyc_sem = 0; return 0;
    }

    /* Cyclic handler — fire every 50ms; do NOT use TA_STA|cycphs=0
     * (that triggers knl_immediate_call_cychdr which runs in task context,
     *  not the real IRQ path we need to exercise for Bug-1 regression).
     * We start it manually via tk_sta_cyc so the first fire comes from
     * the PIT ISR through knl_call_cychdr with ENABLE_INTERRUPT_UPTO. */
    T_CCYC cc;
    cc.exinf   = NULL;
    cc.cycatr  = TA_HLNG;                  /* no TA_STA */
    cc.cychdr  = (FP)st_cyc3_handler;
    cc.cyctim  = 50;
    cc.cycphs  = 50;                       /* first fire 50ms after tk_sta_cyc */
    ID cycid = tk_cre_cyc(&cc);
    if (cycid < E_OK) {
        st_puts("[FAIL] T3: cre_cyc er="); st_puti(cycid); st_puts("\r\n");
        tk_del_sem(st_cyc_sem); st_cyc_sem = 0; return 0;
    }

    tk_sta_cyc(cycid);    /* starts the 50ms countdown through the timer queue */

    /* Wait up to 200ms for the cyclic handler to signal us */
    ER r = tk_wai_sem(st_cyc_sem, 1, 200);

    tk_stp_cyc(cycid);
    tk_del_cyc(cycid);
    tk_del_sem(st_cyc_sem);
    st_cyc_sem = 0;

    if (r != E_OK) {
        st_puts("[FAIL] T3: cyclic->sem er="); st_puti(r); st_puts("\r\n");
        return 0;
    }
    st_puts("[PASS] T3: cyclic->sem (bug1 guard)\r\n");
    return 1;
}

static int run_t4_mutex(void)
{
    /* Semaphore signaled by helper when done */
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 1 };
    st_mtx_done = tk_cre_sem(&cs);
    if (st_mtx_done < E_OK) {
        st_puts("[FAIL] T4: sem er="); st_puti(st_mtx_done); st_puts("\r\n");
        st_mtx_done = 0; return 0;
    }

    /* Mutex */
    T_CMTX cm = { .exinf = NULL, .mtxatr = TA_TFIFO, .ceilpri = 0 };
    st_mtx_id = tk_cre_mtx(&cm);
    if (st_mtx_id < E_OK) {
        st_puts("[FAIL] T4: cre_mtx er="); st_puti(st_mtx_id); st_puts("\r\n");
        tk_del_sem(st_mtx_done); st_mtx_done = 0; st_mtx_id = 0; return 0;
    }

    /* Lock mutex before starting the helper */
    tk_loc_mtx(st_mtx_id, TMO_FEVR);

    /* Create helper at lower priority (15) so it won't preempt init task (pri=1) */
    T_CTSK ct = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                  .task = st_mtx_helper, .itskpri = 15, .stksz = 2048 };
    ID helper = tk_cre_tsk(&ct);
    if (helper < E_OK) {
        st_puts("[FAIL] T4: cre_tsk er="); st_puti(helper); st_puts("\r\n");
        tk_unl_mtx(st_mtx_id); tk_del_mtx(st_mtx_id);
        tk_del_sem(st_mtx_done); st_mtx_id = 0; st_mtx_done = 0; return 0;
    }
    tk_sta_tsk(helper, 0);   /* helper blocks immediately waiting for mutex */

    /* Release mutex — helper becomes ready but init task (pri=1) still runs */
    tk_unl_mtx(st_mtx_id);

    /* Block on semaphore — helper now runs, unlocks mutex, signals done */
    ER r = tk_wai_sem(st_mtx_done, 1, 200);

    tk_del_tsk(helper);      /* task is dormant after tk_ext_tsk */
    tk_del_mtx(st_mtx_id);
    tk_del_sem(st_mtx_done);
    st_mtx_id = 0; st_mtx_done = 0;

    if (r != E_OK) {
        st_puts("[FAIL] T4: mutex 2-task er="); st_puti(r); st_puts("\r\n");
        return 0;
    }
    st_puts("[PASS] T4: mutex 2-task\r\n");
    return 1;
}

static int run_t5_config(void)
{
    int ok = 1;
#if PAGING_MAX_TASKS < (CFN_MAX_TSKID + 1)
    st_puts("[FAIL] T5: PAGING_MAX_TASKS too small\r\n");
    ok = 0;
#endif
#if CFN_MAX_TSKID < 32
    st_puts("[FAIL] T5: CFN_MAX_TSKID < 32\r\n");
    ok = 0;
#endif
    if (ok) st_puts("[PASS] T5: config sanity\r\n");
    return ok;
}

/* ================================================================== */
/* T6: MLP forward determinism                                         */
/* ================================================================== */

static int run_t6_mlp_determinism(void)
{
    B input[MLP_IN] = { 25, 50, 100, 127 };
    UB c1 = mlp_forward(input);
    UB c2 = mlp_forward(input);
    if (c1 != c2) {
        st_puts("[FAIL] T6: mlp non-deterministic c1="); st_puti((W)c1);
        st_puts(" c2="); st_puti((W)c2); st_puts("\r\n");
        return 0;
    }
    if (c1 > 2) {
        st_puts("[FAIL] T6: mlp class out of range c="); st_puti((W)c1); st_puts("\r\n");
        return 0;
    }
    st_puts("[PASS] T6: mlp forward determinism\r\n");
    return 1;
}

/* ================================================================== */
/* T7: MLP output bounds for multiple inputs                           */
/* ================================================================== */

static int run_t7_mlp_bounds(void)
{
    static const B inputs[4][MLP_IN] = {
        {  10, 30,  90, 100 },   /* cold   */
        {  28, 60, 100, 120 },   /* warm   */
        {  40, 80, 110, 127 },   /* hot    */
        { -10, 10,  70,  50 },   /* edge: negative temp */
    };
    for (INT i = 0; i < 4; i++) {
        UB c = mlp_forward(inputs[i]);
        if (c > 2) {
            st_puts("[FAIL] T7: mlp bounds input="); st_puti(i);
            st_puts(" class="); st_puti((W)c); st_puts("\r\n");
            return 0;
        }
    }
    st_puts("[PASS] T7: mlp output bounds\r\n");
    return 1;
}

/* ================================================================== */
/* T8: FedLearn local_train returns E_OK                               */
/* ================================================================== */

static int run_t8_fedlearn_train(void)
{
    static const B samples[2][MLP_IN] = {
        { 10, 30,  90, 100 },
        { 40, 70, 110, 127 },
    };
    static const UB labels[2] = { 0, 2 };

    float dw1[MLP_IN*MLP_H1], db1[MLP_H1];
    float dw2[MLP_H1*MLP_H2], db2[MLP_H2];
    float dw3[MLP_H2*MLP_OUT], db3[MLP_OUT];

    ER r = fl_local_train(samples, labels, 2,
                          dw1, db1, dw2, db2, dw3, db3);
    if (r != E_OK) {
        st_puts("[FAIL] T8: fl_local_train er="); st_puti((W)r); st_puts("\r\n");
        return 0;
    }
    st_puts("[PASS] T8: fedlearn local train\r\n");
    return 1;
}

/* ================================================================== */
/* T9: FedLearn gradient — delta_b3 nonzero after finite-diff          */
/* ================================================================== */

static int run_t9_fedlearn_gradient(void)
{
    static const B samples[3][MLP_IN] = {
        { 10, 30,  90, 100 },
        { 28, 55, 100, 110 },
        { 40, 70, 110, 127 },
    };
    static const UB labels[3] = { 0, 1, 2 };

    float dw1[MLP_IN*MLP_H1], db1[MLP_H1];
    float dw2[MLP_H1*MLP_H2], db2[MLP_H2];
    float dw3[MLP_H2*MLP_OUT], db3[MLP_OUT];

    ER r = fl_local_train(samples, labels, 3,
                          dw1, db1, dw2, db2, dw3, db3);
    if (r != E_OK) {
        st_puts("[FAIL] T9: fl_local_train er="); st_puti((W)r); st_puts("\r\n");
        return 0;
    }
    /* delta_b3 が全ゼロでないことを確認 (finite-diff が実際に動いた証拠) */
    float sum = 0.0f;
    for (INT j = 0; j < MLP_OUT; j++) {
        float v = db3[j];
        sum += (v < 0.0f ? -v : v);
    }
    if (sum == 0.0f) {
        st_puts("[FAIL] T9: fedlearn gradient all-zero\r\n");
        return 0;
    }
    st_puts("[PASS] T9: fedlearn gradient nonzero\r\n");
    return 1;
}

/* ================================================================== */
/* T10: Raft initial state                                             */
/* ================================================================== */

static int run_t10_raft_init_state(void)
{
    /* raft_init() はまだ呼ばれていない — static 初期値を確認 */
    if (raft_role() != RAFT_FOLLOWER) {
        st_puts("[FAIL] T10: raft role not FOLLOWER got=");
        st_puti((W)raft_role()); st_puts("\r\n");
        return 0;
    }
    if (raft_term() != 0) {
        st_puts("[FAIL] T10: raft term not 0 got=");
        st_puti((W)raft_term()); st_puts("\r\n");
        return 0;
    }
    if (raft_leader() != 0xFF) {
        st_puts("[FAIL] T10: raft leader not 0xFF got=");
        st_puti((W)raft_leader()); st_puts("\r\n");
        return 0;
    }
    st_puts("[PASS] T10: raft initial state\r\n");
    return 1;
}

/* ================================================================== */
/* T11: Raft write guard — non-leader must be rejected                 */
/* ================================================================== */

static int run_t11_raft_write_guard(void)
{
    ER r = raft_write(1, 1);
    if (r != E_RSFN) {
        st_puts("[FAIL] T11: raft_write expected E_RSFN got=");
        st_puti((W)r); st_puts("\r\n");
        return 0;
    }
    st_puts("[PASS] T11: raft write guard (non-leader)\r\n");
    return 1;
}

/* ================================================================== */
/* T12: Degrade initial level                                          */
/* ================================================================== */

static int run_t12_degrade_init_level(void)
{
    UB lv = degrade_level();
    if (lv != DEGRADE_FULL) {
        st_puts("[FAIL] T12: degrade level not FULL got=");
        st_puti((W)lv); st_puts("\r\n");
        return 0;
    }
    st_puts("[PASS] T12: degrade initial level FULL\r\n");
    return 1;
}

/* ================================================================== */
/* T13: Tensor alloc / write / read / free                             */
/* ================================================================== */

static int run_t13_tensor_rw(void)
{
    /* tensor_init() は ai_kernel_init() より前に呼ばれるため、ここで初期化。
     * usermain が後で ai_kernel_init() を呼ぶと pool_sem が再生成されるが
     * 旧 sem (1個) はリークする — 許容範囲。 */
    tensor_init();

    UW shape[1] = { 8 };
    ID tid = tk_cre_tensor(1, shape, TENSOR_DTYPE_I8, TENSOR_LAYOUT_FLAT);
    if (tid < 0) {
        st_puts("[FAIL] T13: cre_tensor er="); st_puti((W)tid); st_puts("\r\n");
        return 0;
    }

    static const UB wdata[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    UB rdata[8] = { 0 };

    ER r = tk_tensor_write(tid, 0, wdata, 8);
    if (r != E_OK) {
        st_puts("[FAIL] T13: write er="); st_puti((W)r); st_puts("\r\n");
        tk_del_tensor(tid); return 0;
    }
    r = tk_tensor_read(tid, 0, rdata, 8);
    if (r != E_OK) {
        st_puts("[FAIL] T13: read er="); st_puti((W)r); st_puts("\r\n");
        tk_del_tensor(tid); return 0;
    }
    for (INT i = 0; i < 8; i++) {
        if (rdata[i] != wdata[i]) {
            st_puts("[FAIL] T13: data mismatch idx="); st_puti(i);
            st_puts(" got="); st_puti((W)rdata[i]);
            st_puts(" exp="); st_puti((W)wdata[i]); st_puts("\r\n");
            tk_del_tensor(tid); return 0;
        }
    }
    tk_del_tensor(tid);
    st_puts("[PASS] T13: tensor write/read\r\n");
    return 1;
}

/* ================================================================== */
/* T14: Tensor zero                                                    */
/* ================================================================== */

static int run_t14_tensor_zero(void)
{
    UW shape[1] = { 8 };
    ID tid = tk_cre_tensor(1, shape, TENSOR_DTYPE_I8, TENSOR_LAYOUT_FLAT);
    if (tid < 0) {
        st_puts("[FAIL] T14: cre_tensor er="); st_puti((W)tid); st_puts("\r\n");
        return 0;
    }

    static const UB wdata[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    tk_tensor_write(tid, 0, wdata, 8);
    ER r = tk_tensor_zero(tid);
    if (r != E_OK) {
        st_puts("[FAIL] T14: zero er="); st_puti((W)r); st_puts("\r\n");
        tk_del_tensor(tid); return 0;
    }

    UB rdata[8];
    tk_tensor_read(tid, 0, rdata, 8);
    for (INT i = 0; i < 8; i++) {
        if (rdata[i] != 0) {
            st_puts("[FAIL] T14: not zero idx="); st_puti(i);
            st_puts(" val="); st_puti((W)rdata[i]); st_puts("\r\n");
            tk_del_tensor(tid); return 0;
        }
    }
    tk_del_tensor(tid);
    st_puts("[PASS] T14: tensor zero\r\n");
    return 1;
}

/* ================================================================== */
/* T15: MemStore add / recent retrieval                                */
/* ================================================================== */

static int run_t15_mem_store(void)
{
    /* mem_store_init() は selftest の後に呼ばれる。
     * ring バッファは静的ゼロ初期化済みなので init なしで add/recent が動く。 */
    ER r;
    r = mem_store_add(0, MEM_TYPE_EVENT, "selftest-event-A");
    if (r != E_OK) {
        st_puts("[FAIL] T15: add A er="); st_puti((W)r); st_puts("\r\n");
        return 0;
    }
    r = mem_store_add(0, MEM_TYPE_EVENT, "selftest-event-B");
    if (r != E_OK) {
        st_puts("[FAIL] T15: add B er="); st_puti((W)r); st_puts("\r\n");
        return 0;
    }

    MEM_ENTRY entries[2];
    INT n = mem_recent(0, 2, entries);
    if (n != 2) {
        st_puts("[FAIL] T15: mem_recent n="); st_puti(n); st_puts(" (expect 2)\r\n");
        return 0;
    }
    /* 最新が entries[0] = "selftest-event-B" */
    if (entries[0].text[0] == '\0') {
        st_puts("[FAIL] T15: entries[0] empty\r\n");
        return 0;
    }
    st_puts("[PASS] T15: mem_store add/recent\r\n");
    return 1;
}

/* ================================================================== */
/* Entry point                                                         */
/* ================================================================== */

EXPORT void kernel_selftest(void)
{
    st_puts("\r\n[SELFTEST] kernel built-in self-test\r\n");

    int pass = 0;
    pass += run_t1_sem();
    pass += run_t2_timer();
    pass += run_t3_cyclic_bug1();
    pass += run_t4_mutex();
    pass += run_t5_config();
    pass += run_t6_mlp_determinism();
    pass += run_t7_mlp_bounds();
    pass += run_t8_fedlearn_train();
    pass += run_t9_fedlearn_gradient();
    pass += run_t10_raft_init_state();
    pass += run_t11_raft_write_guard();
    pass += run_t12_degrade_init_level();
    pass += run_t13_tensor_rw();
    pass += run_t14_tensor_zero();
    pass += run_t15_mem_store();

    st_puts("[SELFTEST] ");
    st_puti(pass);
    st_puts("/15 passed");
    if (pass == 15) {
        st_puts(" -- OK\r\n\r\n");
    } else {
        st_puts(" -- KERNEL UNHEALTHY\r\n\r\n");
    }
}
