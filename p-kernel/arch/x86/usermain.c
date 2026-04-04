/*
 *  usermain.c (x86)
 *  p-kernel initial task
 *  Starts: keyboard driver, shell task, RTL8139 NIC, net RX task,
 *          and (if distributed MAC detected) DRPC heartbeat task
 */

#include "kernel.h"
#include "keyboard.h"
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
#include "persist.h"
#include "dtr.h"
#include "dkva.h"
#include "dmn.h"
#include "ga.h"
#include "dproc.h"
#include "mem_store.h"
#include "chat.h"
#include "sfs.h"
#include "pmesh.h"
#include "raft.h"
#include "spawn.h"
#include "moe.h"
#include "ai_kernel.h"
#include "vfs.h"
#include "gdt_user.h"
#include "paging.h"
#include "p_syscall.h"
#include "blk_ssy.h"
#include "fs_ssy.h"
#include "net_ssy.h"
#include <tmonitor.h>

IMPORT void shell_task(INT stacd, void *exinf);

#define SHELL_PRIORITY      2
#define SHELL_STACK         8192
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
#define PERSIST_PRIORITY    10
#define PERSIST_STACK       2048
#define HEAL_ELF_PRIORITY   4
#define HEAL_ELF_STACK      2048
#define DTR_PRIORITY        6
#define DTR_STACK           4096
#define PMESH_PRIORITY      7
#define PMESH_STACK         2048
#define RAFT_PRIORITY       5
#define RAFT_STACK          2048
#define MOE_PRIORITY        8
#define MOE_STACK           2048
#define DKVA_PRIORITY       7
#define DKVA_STACK          4096
#define AI_WORKER_PRIORITY  6
#define AI_WORKER_STACK     4096
#define AI_INFER_PRIORITY   7
#define AI_INFER_STACK      4096
#define DMN_PRIORITY        13   /* 最低優先度 — アイドル時のみ動作 */
#define DMN_STACK           8192 /* GA/推論スタック深度に対応 */

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

IMPORT void kernel_selftest(void);

EXPORT INT usermain(void)
{
    tm_putstring((UB *)"[T-Kernel] Initial task started\r\n");

    kernel_selftest();

    /* ---- Ring-3 userspace infrastructure -------------------------- */
    paging_init();          /* kernel CR3: strip U/S from all PD entries */
    gdt_init_userspace();   /* ring3 GDT entries + 64-bit TSS         */
    syscall_init();         /* INT 0x80 trap gate (DPL=3, CS=0x18)    */
    vfs_init();             /* IDE + FAT32 (optional — ok if no disk) */
    persist_restore_all();  /* Phase 7: ネットワーク前にディスクからトピックを復元 */
    /* NOTE: run_initrc() called after all tasks start (below) */

    /* ---- Subsystems ----------------------------------------------- */
    blk_ssy_init();         /* block device subsystem (ssid=2)         */
    fs_ssy_init();          /* filesystem subsystem  (ssid=3)          */
    net_ssy_init();         /* network subsystem (ssid=1)              */

    /* ---- K-DDS — カーネルネイティブ pub/sub ----------------------- */
    kdds_init();

    /* ---- Phase 7: 永続化タスク + ELF watchdog (ディスクがある場合のみ) */
    if (vfs_ready) {
        if (create_task(persist_task, PERSIST_PRIORITY, PERSIST_STACK) < E_OK)
            tm_putstring((UB *)"[ERR] persist task\r\n");
        else
            tm_putstring((UB *)"[OK]  persist task\r\n");

        heal_elf_init();
        if (create_task(heal_elf_task, HEAL_ELF_PRIORITY, HEAL_ELF_STACK) < E_OK)
            tm_putstring((UB *)"[ERR] heal ELF task\r\n");
        else
            tm_putstring((UB *)"[OK]  heal ELF task\r\n");
    }

    /* ---- AI kernel primitives ------------------------------------- */
    ai_kernel_init();

    /* ---- Phase 8: 分散 Transformer 推論 初期化 ------------------- */
    dtr_init();

    /* ---- Phase 11: 記憶永続化 + AI会話インターフェース ---------- */
    mem_store_init();          /* FAT32 からリングバッファを復元      */
    chat_init();               /* chat コマンド待受け初期化           */

    /* ---- Phase 13: DMN (Default Mode Network) ------------------- */
    dmn_init();

    /* ---- Phase 14: GA (遺伝的アルゴリズム 重み自己改善) --------- */
    ga_init();

    /* ---- Phase 9: 分散プロセスレジストリ ----------------------- */
    dproc_init();

    /* ---- Phase 13: DMN タスク (最低優先度 — アイドル時のみ動作) -- */
    if (create_task(dmn_task, DMN_PRIORITY, DMN_STACK) < E_OK)
        tm_putstring((UB *)"[ERR] DMN task\r\n");
    else
        tm_putstring((UB *)"[OK]  DMN task\r\n");

    /* ---- AI worker task (software NPU) ---------------------------- */
    if (create_task(ai_worker_task, AI_WORKER_PRIORITY, AI_WORKER_STACK) < E_OK)
        tm_putstring((UB *)"[ERR] AI worker task\r\n");
    else
        tm_putstring((UB *)"[OK]  AI worker task\r\n");

    /* ---- AI inference pipeline consumer --------------------------- */
    if (create_task(ai_infer_task, AI_INFER_PRIORITY, AI_INFER_STACK) < E_OK)
        tm_putstring((UB *)"[ERR] AI infer task\r\n");
    else
        tm_putstring((UB *)"[OK]  AI infer task\r\n");

    /* ---- Keyboard ------------------------------------------------- */
    ID kbd_sem = create_sem(0, 64);
    if (kbd_sem < E_OK) {
        tm_putstring((UB *)"[ERR] kbd sem\r\n"); return 0;
    }
    kbd_init(kbd_sem);
    tm_putstring((UB *)"[OK]  Keyboard (IRQ1)\r\n");

    /* ---- Shell task ----------------------------------------------- */
    if (create_task(shell_task, SHELL_PRIORITY, SHELL_STACK) < E_OK) {
        tm_putstring((UB *)"[ERR] shell task\r\n"); return 0;
    }
    tm_putstring((UB *)"[OK]  Shell task\r\n");

    /* ---- RTL8139 NIC ---------------------------------------------- */
    ID net_sem = create_sem(0, 64);
    if (net_sem < E_OK) {
        tm_putstring((UB *)"[ERR] net sem\r\n"); return 0;
    }
    ER er = rtl8139_init(net_sem);
    if (er != E_OK) {
        tm_putstring((UB *)"[WARN] RTL8139 not found (add -device rtl8139)\r\n");
    } else {
        /* ---- Detect distributed mode from MAC --------------------- *
         * Node MACs:  52:54:00:00:00:01 → node 0  IP=10.1.0.1         *
         *             52:54:00:00:00:02 → node 1  IP=10.1.0.2         *
         * Default user-mode MAC: 52:54:00:12:34:56 → single-node mode */
        UB mac[6];
        rtl8139_get_mac(mac);
        if (mac[3] == 0 && mac[4] == 0 && mac[5] >= 1 && mac[5] <= 8) {
            UB nid = (UB)(mac[5] - 1);
            /* IP4(10,1,0,mac[5]) = mac[5]<<24 | 0x0000010A */
            UW nip = ((UW)mac[5] << 24) | 0x0000010AUL;
            drpc_init(nid, nip);
            swim_init();
            heal_init();
            degrade_init();
            heal_register("sensor_pub", 0x0003, 0, 5);
            edf_init();
            replica_init();
            if (create_task(drpc_task, DRPC_PRIORITY, DRPC_STACK) < E_OK)
                tm_putstring((UB *)"[ERR] drpc task\r\n");
            else
                tm_putstring((UB *)"[OK]  DRPC task\r\n");
            if (create_task(swim_task, SWIM_PRIORITY, SWIM_STACK) < E_OK)
                tm_putstring((UB *)"[ERR] SWIM task\r\n");
            else
                tm_putstring((UB *)"[OK]  SWIM task\r\n");
            if (create_task(edf_load_task, EDF_LOAD_PRIORITY, EDF_LOAD_STACK) < E_OK)
                tm_putstring((UB *)"[ERR] EDF load task\r\n");
            else
                tm_putstring((UB *)"[OK]  EDF load task\r\n");
            if (create_task(replica_task, REPLICA_PRIORITY, REPLICA_STACK) < E_OK)
                tm_putstring((UB *)"[ERR] replica task\r\n");
            else
                tm_putstring((UB *)"[OK]  replica task\r\n");
            if (create_task(vital_task, VITAL_PRIORITY, VITAL_STACK) < E_OK)
                tm_putstring((UB *)"[ERR] vital task\r\n");
            else
                tm_putstring((UB *)"[OK]  vital task\r\n");

            /* Phase 8: 分散推論パイプラインタスク */
            if (create_task(dtr_task, DTR_PRIORITY, DTR_STACK) < E_OK)
                tm_putstring((UB *)"[ERR] dtr task\r\n");
            else
                tm_putstring((UB *)"[OK]  dtr task\r\n");

            /* Phase 9.5: 共有フォルダ同期 (SFS) */
            sfs_init();
            sfs_boot_sync();   /* 全ノードへ /shared/ の内容を要求 */

            /* Phase 10 前準備: メッシュルーティング */
            pmesh_init();
            if (create_task(pmesh_task, PMESH_PRIORITY, PMESH_STACK) < E_OK)
                tm_putstring((UB *)"[ERR] pmesh task\r\n");
            else
                tm_putstring((UB *)"[OK]  pmesh task\r\n");

            /* Phase 10: 分散 KV Attention */
            dkva_init();
            if (create_task(dkva_task, DKVA_PRIORITY, DKVA_STACK) < E_OK)
                tm_putstring((UB *)"[ERR] dkva task\r\n");
            else
                tm_putstring((UB *)"[OK]  dkva task\r\n");

            /* Phase 10: Raft コンセンサス */
            raft_init();
            if (create_task(raft_task, RAFT_PRIORITY, RAFT_STACK) < E_OK)
                tm_putstring((UB *)"[ERR] raft task\r\n");
            else
                tm_putstring((UB *)"[OK]  raft task\r\n");

            /* Phase 10: 自己増殖 */
            spawn_init();

            /* Phase 10: MoE 推論ルーティング */
            moe_init();
            if (create_task(moe_task, MOE_PRIORITY, MOE_STACK) < E_OK)
                tm_putstring((UB *)"[ERR] moe task\r\n");
            else
                tm_putstring((UB *)"[OK]  moe task\r\n");

        }

        /* Send initial ARP from here (priority 1) so the reply arrives
         * before the shell task has a chance to run. */
        netstack_start();

        /* ---- Net RX task ------------------------------------------ */
        if (create_task(net_task, NET_PRIORITY, NET_STACK) < E_OK) {
            tm_putstring((UB *)"[ERR] net task\r\n");
        } else {
            tm_putstring((UB *)"[OK]  Net RX task\r\n");
        }
    }

    return 0;
}
