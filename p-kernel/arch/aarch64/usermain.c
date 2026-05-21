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

    raft_init();
    try_task((FP)raft_task, RAFT_PRIORITY, RAFT_STACK, "raft task");

    spawn_init();

    moe_init();
    try_task((FP)moe_task, MOE_PRIORITY, MOE_STACK, "moe task");

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

    /* AI primitives — tensor / ai_job / pipeline / MLP seed */
    ai_kernel_init();

    /* K-DDS topic table + distributed Transformer pools. Init here so
     * single-node mode can publish/subscribe even before NIC bring-up. */
    kdds_init();
    dtr_init();

    print("\r\nCommands: ai (show stats) | net (init RTL8139) | echo: any text\r\n");
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
