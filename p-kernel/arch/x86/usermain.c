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
#include <tmonitor.h>

IMPORT void shell_task(INT stacd, void *exinf);

#define SHELL_PRIORITY   2
#define SHELL_STACK      8192
#define NET_PRIORITY     3
#define NET_STACK        4096
#define DRPC_PRIORITY    5
#define DRPC_STACK       4096

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

EXPORT INT usermain(void)
{
    tm_putstring((UB *)"[T-Kernel] Initial task started\r\n");

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
            if (create_task(drpc_task, DRPC_PRIORITY, DRPC_STACK) < E_OK)
                tm_putstring((UB *)"[ERR] drpc task\r\n");
            else
                tm_putstring((UB *)"[OK]  DRPC task\r\n");
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
