/*
 *  usermain.c (aarch64)
 *  Initial task — boots the AI kernel primitives, brings up the RTL8139,
 *  and (when the MAC matches the cluster convention) joins the swarm.
 *
 *  MAC convention:
 *    52:54:00:00:00:0N → distributed node N (N=1..8)
 *                          → IP 10.1.0.N, node_id = N-1
 *                          → DRPC / SWIM / K-DDS / replica / pmesh / Raft
 *                            / DKVA / MoE / kloader / SFS all come online.
 *    52:54:00:12:34:56 (QEMU default) → single-node AI shell only.
 */

#include "kernel.h"
#include <tmonitor.h>
#include "ai_kernel.h"
#include "rtl8139.h"
#include "netstack.h"
#include "drpc.h"
#include "swim.h"
#include "kdds.h"
#include "heal.h"
#include "degrade.h"
#include "edf.h"
#include "replica.h"
#include "vital.h"
#include "dtr.h"
#include "dkva.h"
#include "sfs.h"
#include "pmesh.h"
#include "raft.h"
#include "spawn.h"
#include "moe.h"
#include "world.h"
#include "kloader_task.h"

IMPORT void sio_send_frame(const UB *buf, INT size);
IMPORT INT  sio_read_line(UB *buf, INT maxlen);
IMPORT void ai_stats_print(void);

static void print(const char *s)
{
    sio_send_frame((const UB *)s, (INT)__builtin_strlen(s));
}

static int strneq(const UB *a, const char *b, INT n)
{
    for (INT i = 0; i < n; i++) {
        if (a[i] != (UB)b[i]) return 0;
        if (b[i] == 0) return 1;
    }
    return 1;
}

/* Task priorities + stack sizes mirror arch/x86/usermain.c so the
 * cluster has identical scheduling behaviour across architectures. */
#define NET_PRIORITY        3
#define NET_STACK           4096
#define DRPC_PRIORITY       5
#define DRPC_STACK          4096
#define SWIM_PRIORITY       6
#define SWIM_STACK          4096
#define EDF_LOAD_PRIORITY   7
#define EDF_LOAD_STACK      2048
#define REPLICA_PRIORITY    8
#define REPLICA_STACK       2048
#define VITAL_PRIORITY      9
#define VITAL_STACK         2048
#define DTR_PRIORITY        6
#define DTR_STACK           4096
#define PMESH_PRIORITY      7
#define PMESH_STACK         2048
#define DKVA_PRIORITY       7
#define DKVA_STACK          4096
#define RAFT_PRIORITY       5
#define RAFT_STACK          2048
#define MOE_PRIORITY        8
#define MOE_STACK           2048
#define WORLD_PRIORITY      7
#define WORLD_STACK         4096
#define KLOADER_PRIORITY    9
#define KLOADER_STACK       4096

static ID create_sem(INT isemcnt, INT maxsem)
{
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO,
                  .isemcnt = isemcnt, .maxsem = maxsem };
    return tk_cre_sem(&cs);
}

static ID create_task(FP fn, INT pri, INT stksz)
{
    T_CTSK ct = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
                  .task = fn, .itskpri = pri, .stksz = stksz };
    ID id = tk_cre_tsk(&ct);
    if (id >= E_OK) tk_sta_tsk(id, 0);
    return id;
}

static void try_task(FP fn, INT pri, INT stksz, const char *name)
{
    if (create_task(fn, pri, stksz) < E_OK) {
        print("[ERR] ");
    } else {
        print("[OK]  ");
    }
    print(name);
    print("\r\n");
}

static void distributed_init(UB nid, UW nip)
{
    print("[dist] joining cluster as node ");
    {
        char d[2] = { (char)('0' + nid), '\0' };
        print(d);
    }
    print("\r\n");

    drpc_init(nid, nip);
    swim_init();
    heal_init();
    degrade_init();
    heal_register("sensor_pub", 0x0003, 0, 5);
    edf_init();
    replica_init();

    try_task((FP)drpc_task,     DRPC_PRIORITY,     DRPC_STACK,     "DRPC task");
    try_task((FP)swim_task,     SWIM_PRIORITY,     SWIM_STACK,     "SWIM task");
    try_task((FP)edf_load_task, EDF_LOAD_PRIORITY, EDF_LOAD_STACK, "EDF load task");
    try_task((FP)replica_task,  REPLICA_PRIORITY,  REPLICA_STACK,  "replica task");
    try_task((FP)vital_task,    VITAL_PRIORITY,    VITAL_STACK,    "vital task");

    /* Phase 8: distributed Transformer inference */
    try_task((FP)dtr_task, DTR_PRIORITY, DTR_STACK, "dtr task");

    /* Phase 9.5: shared filesystem — SFS is a no-op without VFS, but
     * the init+broadcast logic is harmless. */
    sfs_init();
    sfs_boot_sync();

    /* Phase 10: mesh routing + KV attention + Raft + MoE + spawn */
    pmesh_init();
    try_task((FP)pmesh_task, PMESH_PRIORITY, PMESH_STACK, "pmesh task");

    dkva_init();
    try_task((FP)dkva_task, DKVA_PRIORITY, DKVA_STACK, "dkva task");

    /* Raft は BOOT サービスにしない (NOCENTRAL-RAFT, wave-55)。
     * Raft リーダー = 特権的な中央コーディネータ → 「中央なし」のテーゼに反する。
     * linux ビルドに合わせて raft_init()/raft_task の boot 起動を撤去。
     * 中央調停の代わりに swim/world の分散スタックが走る (下記 world_task)。
     * Raft は legacy/optional のデモとして残り、シェルの `raft` verb で
     * 必要時に遅延起動できる (本ファイル下部の raft 処理を参照)。 */

    /* spawn プロトコルのポート bind のみ。旧来の自動配布は raft リーダー
     * 選出時にのみ発火していた (spawn_on_leader) — raft を boot しない今は
     * 自動発火しない (linux ビルドと同じ)。 */
    spawn_init();

    moe_init();
    try_task((FP)moe_task, MOE_PRIORITY, MOE_STACK, "moe task");

    /* World map — self-beacon + gossip 取り込み常駐タスク。
     * 全ノードで同一に走る (中央なし — world.h NO-CENTRAL 不変条件) */
    try_task((FP)world_task, WORLD_PRIORITY, WORLD_STACK, "world task");

    /* OTA: kloader receives KLOAD frames + auto-pushes to bare nodes. */
    kloader_task_init();
    try_task((FP)kloader_task, KLOADER_PRIORITY, KLOADER_STACK, "kloader task");
}

static void net_bringup(void)
{
    static INT net_up = 0;
    if (net_up) {
        print("[net] already up\r\n");
        return;
    }

    ID net_sem = create_sem(0, 64);
    if (net_sem < E_OK) {
        print("[net] sem create failed\r\n");
        return;
    }

    ER er = rtl8139_init(net_sem);
    if (er != E_OK) {
        print("[net] init failed (add -device rtl8139?)\r\n");
        return;
    }

    /* MAC-based cluster role detection. */
    UB mac[6];
    rtl8139_get_mac(mac);
    if (mac[3] == 0 && mac[4] == 0 && mac[5] >= 1 && mac[5] <= 8) {
        UB nid = (UB)(mac[5] - 1);
        UW nip = ((UW)mac[5] << 24) | 0x0000010AUL;   /* 10.1.0.N */
        distributed_init(nid, nip);
    } else {
        print("[net] single-node mode (no cluster MAC)\r\n");
    }

    /* Always: emit an ARP so neighbours can find us. */
    netstack_start();

    try_task((FP)net_task, NET_PRIORITY, NET_STACK, "Net RX task");

    net_up = 1;
}

EXPORT INT usermain(void)
{
    print("\r\n");
    print("====================================\r\n");
#ifdef BOARD_RPI3
    print(" p-kernel  [aarch64 / RPi 3 BCM2837]\r\n");
#else
    print(" p-kernel  [aarch64 / QEMU virt]\r\n");
#endif
    print(" Phase 2c: AI + distributed kernel\r\n");
    print("====================================\r\n");

#ifdef SMP_2TASKS_PROD
    /* ②.2a [smp-2tasks-prod]: prove the PRODUCTION scheduler runs TWO REAL
     * T-Kernel TCBs on TWO DISTINCT CPUs under the BKL.  Runs here, inside the
     * initial task, with the kernel fully up (real TCBs + ready queue exist).
     * Prints a verdict the harness greps. */
    {
        extern int  smp_prod_test_run(void);
        extern void *smp_prod_a_tcb(void);
        extern void *smp_prod_b_tcb(void);
        extern unsigned long smp_prod_b_ran(void);
        extern unsigned long smp_prod_b_loops(void);
        extern int  smp_prod_b_tskid(void);

        /* minimal 16-hex-digit printer for the TCB pointers (evidence) */
        char hx[19]; hx[0]='0'; hx[1]='x'; hx[18]='\0';
        #define PRINT_PTR(p) do { unsigned long _v=(unsigned long)(p); \
            for (int _i=0;_i<16;_i++){int _n=(int)((_v>>((15-_i)*4))&0xF); \
            hx[2+_i]=(char)(_n<10?'0'+_n:'a'+_n-10);} print(hx); } while(0)

        print("[SMP] ②.2a [smp-2tasks-prod]: 2 REAL TCBs on 2 CPUs under the BKL...\r\n");
        int rc = smp_prod_test_run();

        void *a = smp_prod_a_tcb();
        void *b = smp_prod_b_tcb();
        print("[SMP] cpu0 ctxtsk(A)="); PRINT_PTR(a);
        print(" cpu1 ctxtsk(B)=");      PRINT_PTR(b);
        print("\r\n");
        print("[SMP] B real tskid=");
        { int t=smp_prod_b_tskid(); char d[4]; int o=0; if(t<0){d[o++]='-';t=-t;}
          char tmp[4]; int ti=0; if(t==0)tmp[ti++]='0'; while(t){tmp[ti++]=(char)('0'+t%10);t/=10;}
          while(ti)d[o++]=tmp[--ti]; d[o]='\0'; print(d); }
        print(" B ran=");  print(smp_prod_b_ran()?"1":"0");
        print(" B loops>0=");  print(smp_prod_b_loops()?"1":"0");
        print("\r\n");

        if (rc == 0 && a && b && a != b && smp_prod_b_ran()) {
            print("SMP-2TASKS-PROD: PASS\r\n");
        } else {
            print("SMP-2TASKS-PROD: FAIL rc=");
            { int t=rc; char d[8]; int o=0; if(t<0){d[o++]='-';t=-t;}
              char tmp[8]; int ti=0; if(t==0)tmp[ti++]='0'; while(t){tmp[ti++]=(char)('0'+t%10);t/=10;}
              while(ti)d[o++]=tmp[--ti]; d[o]='\0'; print(d); }
            print(" (need 2 distinct real TCBs on 2 CPUs)\r\n");
        }
        #undef PRINT_PTR
    }
#endif /* SMP_2TASKS_PROD */

#ifdef SMP_ONE_MIND
    /* ②.2c [smp-one-mind] CROWN: prove a REAL mind forward (r_forward) is
     * BYTE-IDENTICAL whether run uniprocessor (H_uni, on CPU 0) or as a real
     * task on a SECONDARY CPU under SMP (H_smp, on CPU 1, via the ②.2a smp_prod
     * pattern).  H_uni == H_smp ⇒ the SMP scheduler did not perturb a single bit
     * of the mind's math — "the mind stays one across the SMP scheduler."  Runs
     * here, inside the initial task, kernel fully up (real TCBs + ready queue).
     * FALSIFIER -DSMP_ONEMIND_RACE: a 2nd CPU scribbles the shared rc/rw[] while
     * CPU 1's forward is mid-flight → H_smp != H_uni → FAIL hashes-differ.
     *
     * HONEST SCOPE: the crown is NARROW + TRUE — a SINGLE forward survives SMP
     * scheduling bit-for-bit.  CONCURRENT minds (two forwards / forward+train)
     * race the SHARED static rc/rw[] and need a mind-lock — DEFERRED (that is
     * exactly what the falsifier exploits).  ②.2b-ii (secondary timer/WAIT) is
     * NOT needed: the forward is run-to-completion + parks on wfe. */
    {
        extern int  smp_onemind_test_run(void);
        extern unsigned long smp_onemind_h_uni(void);
        extern unsigned long smp_onemind_h_smp(void);
        extern unsigned long smp_onemind_m_ran(void);
        extern void *smp_onemind_m_tcb(void);
        extern int  smp_onemind_m_tskid(void);
        extern unsigned long smp_onemind_filler(int cpu);

        /* 16-hex-digit printer for the 64-bit FNV hashes */
        char hx[19]; hx[0]='0'; hx[1]='x'; hx[18]='\0';
        #define PRINT_HASH(p) do { unsigned long _v=(unsigned long)(p); \
            for (int _i=0;_i<16;_i++){int _n=(int)((_v>>((15-_i)*4))&0xF); \
            hx[2+_i]=(char)(_n<10?'0'+_n:'a'+_n-10);} print(hx); } while(0)

        print("[SMP] ②.2c [smp-one-mind] CROWN: r_forward byte-identical uniproc vs SMP-secondary...\r\n");
        int rc = smp_onemind_test_run();

        unsigned long h_uni = smp_onemind_h_uni();
        unsigned long h_smp = smp_onemind_h_smp();

        print("[SMP] M real tskid=");
        { int t=smp_onemind_m_tskid(); char d[6]; int o=0; if(t<0){d[o++]='-';t=-t;}
          char tmp[6]; int ti=0; if(t==0)tmp[ti++]='0'; while(t){tmp[ti++]=(char)('0'+t%10);t/=10;}
          while(ti)d[o++]=tmp[--ti]; d[o]='\0'; print(d); }
        print(" M ran="); print(smp_onemind_m_ran()?"1":"0");
        print(" M tcb=ok="); print(smp_onemind_m_tcb()?"1":"0");
        print("\r\n");
        /* concurrency proof: the non-mind secondaries advanced their fillers */
        print("[SMP] concurrency: cpu2_filler>0=");
        print(smp_onemind_filler(2)?"1":"0");
        print(" cpu3_filler>0=");
        print(smp_onemind_filler(3)?"1":"0");
        print("\r\n");
        print("[SMP] H_uni="); PRINT_HASH(h_uni);
        print(" (uniprocessor, CPU 0)\r\n");
        print("[SMP] H_smp="); PRINT_HASH(h_smp);
        print(" (SMP secondary, CPU 1)\r\n");

        if (rc == 0 && h_uni == h_smp && smp_onemind_m_ran()) {
            print("SMP-ONE-MIND: PASS H_uni="); PRINT_HASH(h_uni);
            print(" H_smp="); PRINT_HASH(h_smp);
            print(" (the mind is byte-identical under SMP; single-forward scope, concurrent minds DEFERRED)\r\n");
        } else if (rc == -99 || (h_uni != h_smp && smp_onemind_m_ran())) {
            /* The split-mind result: hashes DIFFER.  In the -DSMP_ONEMIND_RACE
             * falsifier this is the EXPECTED RED (the shared-rc race is load-
             * bearing).  In a guard'd (non-race) build this would be a REAL
             * split-mind BUG — do NOT paper it. */
            print("SMP-ONE-MIND: FAIL hashes-differ H_uni="); PRINT_HASH(h_uni);
            print(" H_smp="); PRINT_HASH(h_smp);
            print("\r\n");
        } else {
            print("SMP-ONE-MIND: FAIL rc=");
            { int t=rc; char d[8]; int o=0; if(t<0){d[o++]='-';t=-t;}
              char tmp[8]; int ti=0; if(t==0)tmp[ti++]='0'; while(t){tmp[ti++]=(char)('0'+t%10);t/=10;}
              while(ti)d[o++]=tmp[--ti]; d[o]='\0'; print(d); }
            print(" (M did not run on CPU 1 / plumbing failure)\r\n");
        }
        #undef PRINT_HASH
    }
#endif /* SMP_ONE_MIND */

#ifdef SMP_ASYNC_PREEMPT
    /* ②.2b-i [smp-async-preempt]: prove a REAL task on the SECONDARY is
     * preempted ASYNCHRONOUSLY MID-LOOP (no flag-check) by an SGI whose
     * IRQ-return path performs a REAL register-context switch, and RESUMES
     * correctly (its loop counter continues).  Runs here inside the initial
     * task with the kernel fully up (real TCBs + ready queue exist).  Prints a
     * verdict the harness greps.  FALSIFIER -DSMP_NO_ASYNC: no IRQ-return
     * switch → L never preempted → FAIL (with sgi_taken>=1). */
    {
        extern int  smp_async_test_run(void);
        extern unsigned long smp_async_counter(void);
        extern unsigned long smp_async_observed(void);
        extern unsigned long smp_async_final(void);
        extern unsigned long smp_async_highprio_ran(void);
        extern unsigned long smp_async_loop_cap(void);
        extern unsigned long smp_sgi_taken(int cpu);

        /* minimal unsigned-long decimal printer (evidence) */
        #define PRINT_ULONG(v) do { unsigned long _u=(unsigned long)(v); \
            char _t[21]; int _i=0; if(_u==0)_t[_i++]='0'; \
            while(_u){_t[_i++]=(char)('0'+_u%10);_u/=10;} \
            char _o[22]; int _j=0; while(_i)_o[_j++]=_t[--_i]; _o[_j]='\0'; \
            print(_o); } while(0)
        #define PRINT_SLONG(v) do { long _s=(long)(v); if(_s<0){print("-");_s=-_s;} \
            PRINT_ULONG((unsigned long)_s); } while(0)

        print("[SMP] ②.2b-i [smp-async-preempt]: async MID-LOOP preempt + resume...\r\n");
        int rc = smp_async_test_run();

        unsigned long obs   = smp_async_observed();
        unsigned long fin   = smp_async_final();
        unsigned long cap   = smp_async_loop_cap();
        unsigned long hr    = smp_async_highprio_ran();
        unsigned long sgis  = smp_sgi_taken(1);

        print("[SMP] cpu1 highprio_ran="); PRINT_ULONG(hr);
        print(" sgi_taken=");              PRINT_ULONG(sgis); print("\r\n");
        print("[SMP] observed_counter(at preempt)="); PRINT_ULONG(obs);
        print(" final_counter=");                     PRINT_ULONG(fin);
        print(" cap=");                               PRINT_ULONG(cap); print("\r\n");

        /* PASS: H ran on CPU 1, the preempt landed MID-loop (0 < observed <
         * cap), L RESUMED + finished (final == cap), the counter CONTINUED
         * (final > observed), and an SGI was actually taken (sgi_taken>=1). */
        if (rc == 0 && hr == 1 && obs > 0 && obs < cap &&
            fin == cap && fin > obs && sgis >= 1) {
            print("SMP-ASYNC-PREEMPT: PASS\r\n");
        } else {
            print("SMP-ASYNC-PREEMPT: FAIL rc="); PRINT_SLONG(rc);
            print(" (no mid-loop preempt / no resume; need hr=1, 0<obs<cap, fin=cap, sgi>=1)\r\n");
        }
        #undef PRINT_ULONG
        #undef PRINT_SLONG
    }
#endif /* SMP_ASYNC_PREEMPT */

#ifdef SMP_DEADLOCK_TEST
    /* ②.2b [smp-no-deadlock]: prove the §5.4 BKL-held reschedule guard is
     * LOAD-BEARING.  A REAL task L on the SECONDARY ACQUIRES the BKL (enters a
     * kernel critical section) and spins a tight loop INSIDE it; an SGI for a
     * higher-prio task H lands mid-critical-section.  WITH the guard: the switch
     * is DEFERRED, L's critical section completes ATOMICALLY (observed_crit ==
     * cap), L releases the BKL, the DEFERRED reschedule (pending flag KEPT) fires
     * → H runs + acquires the BKL cleanly → no deadlock.  FALSIFIER
     * -DSMP_NO_BKL_GUARD removes the guard → the switch fires WHILE L holds the
     * BKL → H's bkl_acquire spins forever → DEADLOCK → watchdog → FAIL. */
    {
        extern int  smp_dl_test_run(void);
        extern unsigned long smp_dl_observed_crit(void);
        extern unsigned long smp_dl_obs_released(void);
        extern unsigned long smp_dl_crit_cap(void);
        extern unsigned long smp_dl_post(void);
        extern unsigned long smp_dl_highprio_ran(void);
        extern unsigned long smp_dl_hi_got_bkl(void);
        extern unsigned long smp_dl_resumed(void);
        extern unsigned long smp_sgi_taken(int cpu);

        #define PRINT_ULONG(v) do { unsigned long _u=(unsigned long)(v); \
            char _t[21]; int _i=0; if(_u==0)_t[_i++]='0'; \
            while(_u){_t[_i++]=(char)('0'+_u%10);_u/=10;} \
            char _o[22]; int _j=0; while(_i)_o[_j++]=_t[--_i]; _o[_j]='\0'; \
            print(_o); } while(0)
        #define PRINT_SLONG(v) do { long _s=(long)(v); if(_s<0){print("-");_s=-_s;} \
            PRINT_ULONG((unsigned long)_s); } while(0)

        print("[SMP] ②.2b [smp-no-deadlock]: BKL-held mid-crit preempt is DEFERRED...\r\n");
        int rc = smp_dl_test_run();

        unsigned long oc   = smp_dl_observed_crit();
        unsigned long orl  = smp_dl_obs_released();
        unsigned long cap  = smp_dl_crit_cap();
        unsigned long post = smp_dl_post();
        unsigned long hr   = smp_dl_highprio_ran();
        unsigned long gb   = smp_dl_hi_got_bkl();
        unsigned long rsm  = smp_dl_resumed();
        unsigned long sgis = smp_sgi_taken(1);

        print("[SMP] cpu1 highprio_ran="); PRINT_ULONG(hr);
        print(" hi_got_bkl=");             PRINT_ULONG(gb);
        print(" sgi_taken=");              PRINT_ULONG(sgis); print("\r\n");
        print("[SMP] observed_crit(at H)="); PRINT_ULONG(oc);
        print(" crit_cap=");                 PRINT_ULONG(cap);
        print(" obs_released=");             PRINT_ULONG(orl);
        print(" post=");                     PRINT_ULONG(post);
        print(" resumed=");                  PRINT_ULONG(rsm); print("\r\n");

        /* PASS: the deferred reschedule fired (hr=1), the critical section was
         * ATOMIC (obs_released=1 → L finished + RELEASED the BKL before H ran),
         * the BKL was NOT stranded (hi_got_bkl=1), L RESUMED (resumed=1,
         * post==cap), and an SGI was delivered (sgi>=1). */
        if (rc == 0 && hr == 1 && orl == 1 && gb == 1 && rsm == 1 &&
            post == cap && sgis >= 1) {
            print("SMP-NO-DEADLOCK: PASS\r\n");
        } else {
            print("SMP-NO-DEADLOCK: FAIL rc="); PRINT_SLONG(rc);
            print(" (deadlock / deferred reschedule lost / crit not atomic; "
                  "need hr=1, obs_released=1, hi_got_bkl=1, resumed=1, post=cap, sgi>=1)\r\n");
        }
        #undef PRINT_ULONG
        #undef PRINT_SLONG
    }
#endif /* SMP_DEADLOCK_TEST */

    /* AI primitives — tensor / ai_job / pipeline / MLP seed */
    ai_kernel_init();

    /* K-DDS topic table + distributed Transformer pools. Init here so
     * single-node mode can publish/subscribe even before NIC bring-up. */
    kdds_init();
    dtr_init();

    /* World map — decentralized situational awareness. Init here so the
     * `world` command works even single-node; the beacon task starts in
     * distributed_init() once the node ID is known. */
    world_init();

    print("\r\nCommands: ai (show stats) | net (init RTL8139) | world/map (network map) | raft (optional consensus demo) | echo: any text\r\n");
    print("p-kernel> ");

    for (;;) {
        UB line[128];
        INT n = sio_read_line(line, sizeof(line));
        if (n == 0) {
            print("\r\np-kernel> ");
            continue;
        }
        print("\r\n");
        if (n >= 2 && strneq(line, "ai", 2)) {
            ai_stats_print();
        } else if (n >= 3 && strneq(line, "net", 3)) {
            net_bringup();
        } else if (n >= 3 && strneq(line, "arp", 3)) {
            arp_dump();
        } else if ((n >= 5 && strneq(line, "world", 5)) ||
                   (n >= 3 && strneq(line, "map", 3))) {
            world_print();
        } else if (n >= 4 && strneq(line, "raft", 4)) {
            /* Raft は boot サービスではない (NOCENTRAL-RAFT)。
             * 必要時にのみ遅延起動する legacy/optional のデモ。
             * 初回だけ init + コンセンサスタスクを起動する。 */
            static INT raft_started = 0;
            if (!raft_started) {
                raft_started = 1;
                raft_init();
                try_task((FP)raft_task, RAFT_PRIORITY, RAFT_STACK,
                         "raft task (on-demand)");
                print("[raft] on-demand consensus started (legacy/optional)\r\n");
            }
            raft_stat();
        } else if (n >= 2 && strneq(line, "rx", 2)) {
            extern unsigned long rtl_mmio_for_diag;
            extern volatile UW net_rx_arp, net_rx_udp, net_rx_icmp_req, net_rx_tcp;
            char buf2[160];
            UH isr = 0;
            if (rtl_initialized) {
                isr = *(volatile UH *)(rtl_mmio_for_diag + 0x3E);
            }
            INT i = 0;
            static const char hex[] = "0123456789ABCDEF";
            #define APPEND_STR(s) do { const char *p = s; while (*p) buf2[i++] = *p++; } while (0)
            #define APPEND_DEC(v) do { UW vv = (v); if (vv == 0) buf2[i++] = '0'; \
                else { char tmp[12]; INT t = 0; while (vv > 0) { tmp[t++] = '0' + (vv % 10); vv /= 10; } \
                while (t > 0) buf2[i++] = tmp[--t]; } } while (0)
            APPEND_STR("[rx] frame="); APPEND_DEC(rtl_rx_count);
            APPEND_STR("  tx="); APPEND_DEC(rtl_tx_count);
            APPEND_STR("  ISR=0x");
            buf2[i++] = hex[(isr >> 12) & 0xF];
            buf2[i++] = hex[(isr >>  8) & 0xF];
            buf2[i++] = hex[(isr >>  4) & 0xF];
            buf2[i++] = hex[isr & 0xF];
            APPEND_STR("\r\n[rx] arp="); APPEND_DEC(net_rx_arp);
            APPEND_STR("  icmp_req="); APPEND_DEC(net_rx_icmp_req);
            APPEND_STR("  udp="); APPEND_DEC(net_rx_udp);
            APPEND_STR("  tcp="); APPEND_DEC(net_rx_tcp);
            extern volatile UW net_eth_in, net_eth_unknown, net_ip_in;
            extern volatile UW net_ip_drop_size, net_ip_drop_vhl, net_ip_drop_csum, net_ip_drop_dst;
            APPEND_STR("\r\n[rx] eth_in="); APPEND_DEC(net_eth_in);
            APPEND_STR("  eth_unk="); APPEND_DEC(net_eth_unknown);
            APPEND_STR("  ip_in="); APPEND_DEC(net_ip_in);
            APPEND_STR("\r\n[rx] ip drops: size="); APPEND_DEC(net_ip_drop_size);
            APPEND_STR(" vhl="); APPEND_DEC(net_ip_drop_vhl);
            APPEND_STR(" csum="); APPEND_DEC(net_ip_drop_csum);
            APPEND_STR(" dst="); APPEND_DEC(net_ip_drop_dst);
            buf2[i++] = '\r'; buf2[i++] = '\n';
            sio_send_frame((const UB *)buf2, i);
            #undef APPEND_STR
            #undef APPEND_DEC
        } else {
            print("[echo] ");
            sio_send_frame(line, n);
            print("\r\n");
        }
        print("p-kernel> ");
    }

    return 0;
}
