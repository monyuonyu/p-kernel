/*
 * kserve.c — Kernel Serve daemon (Phase 16b)
 *
 * Listens on UDP port 7370 for KLRQ ("Kernel Load Request") broadcasts
 * sent by kloader.bin running on a diskless node.  On receipt, reads
 * kernel.elf from the local FAT32 filesystem and transmits it in
 * 1 KB chunks using the KLRS / KLRD / KLRE protocol.
 *
 * Protocol:
 *   Receive: "KLRQ" + client_ip[4] + client_mac[6]
 *   Send:    "KLRS" + session_id[4] + total_size[4]
 *            "KLRD" + session_id[4] + offset[4] + len[2] + data[len]  (×N)
 *            "KLRE" + session_id[4] + total_size[4]
 *
 *   session_id = net_my_ip (sender's IP as uint32, little-endian)
 *   Chunk size: KSERVE_CHUNK (1024 bytes)
 *
 * The client's MAC is registered in the ARP table so udp_send() can
 * build a correct unicast Ethernet frame directly to the booting node.
 *
 * kserve_init() is called from usermain() during startup.
 */

#include "kernel.h"
#include <tmonitor.h>
#include "netstack.h"
#include "vfs.h"

/* Kernel ELF path on the local filesystem */
#define KSERVE_KERNEL_PATH  "/kernel.elf"
#define KSERVE_PORT         7370
#define KSERVE_CHUNK        1024

/* ------------------------------------------------------------------ */
/* Task state                                                          */
/* ------------------------------------------------------------------ */

static ID  kserve_sem  = 0;
static UW  kserve_client_ip  = 0;

/* ------------------------------------------------------------------ */
/* UDP receive callback (called from net_task context)                */
/* ------------------------------------------------------------------ */

static void kserve_recv(UW src_ip, UH src_port, const UB *data, UH len)
{
    (void)src_port;
    if (len < 14) return;
    if (data[0]!='K'||data[1]!='L'||data[2]!='R'||data[3]!='Q') return;

    /* Extract client IP and MAC from payload */
    UW client_ip;
    UB client_mac[6];
    __builtin_memcpy(&client_ip,   data+4, 4);
    __builtin_memcpy(client_mac,   data+8, 6);

    /* If src_ip is non-zero, prefer it; otherwise use payload IP */
    kserve_client_ip = src_ip ? src_ip : client_ip;

    /* Seed ARP table so udp_send can resolve the MAC without ARP request */
    arp_seed(kserve_client_ip, client_mac);

    /* Signal the serve task (non-blocking: if already pending, ignore) */
    tk_sig_sem(kserve_sem, 1);
}

/* ------------------------------------------------------------------ */
/* Serve task                                                          */
/* ------------------------------------------------------------------ */

static void kserve_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    static UB chunk[KSERVE_CHUNK + 14];   /* KLRD header(14) + data */
    static UB meta[12];                   /* KLRS / KLRE            */

    while (1) {
        /* Wait for a KLRQ */
        tk_wai_sem(kserve_sem, 1, TMO_FEVR);

        UW client_ip = kserve_client_ip;

        /* ---- Open kernel.elf from VFS ---------------------------- */
        if (!vfs_ready) {
            tm_putstring((UB *)"[kserve] VFS not ready\r\n");
            continue;
        }
        INT fd = vfs_open(KSERVE_KERNEL_PATH);
        if (fd < 0) {
            tm_putstring((UB *)"[kserve] kernel.elf not found\r\n");
            continue;
        }

        UW total = vfs_fsize(fd);
        if (total == 0) { vfs_close(fd); continue; }

        UW session_id = (UW)NET_MY_IP;

        tm_putstring((UB *)"[kserve] sending kernel.elf to ");
        tm_putstring((UB *)ip_str(client_ip));
        tm_putstring((UB *)"\r\n");

        /* ---- KLRS ------------------------------------------------- */
        meta[0]='K'; meta[1]='L'; meta[2]='R'; meta[3]='S';
        __builtin_memcpy(meta+4, &session_id, 4);
        __builtin_memcpy(meta+8, &total,      4);
        udp_send(client_ip, KSERVE_PORT, KSERVE_PORT, meta, 12);

        /* ---- KLRD chunks ------------------------------------------ */
        UW offset = 0;
        while (offset < total) {
            INT n = vfs_read(fd, chunk+14, KSERVE_CHUNK);
            if (n <= 0) break;

            UH ulen = (UH)n;
            chunk[0]='K'; chunk[1]='L'; chunk[2]='R'; chunk[3]='D';
            __builtin_memcpy(chunk+4,  &session_id, 4);
            __builtin_memcpy(chunk+8,  &offset,     4);
            __builtin_memcpy(chunk+12, &ulen,        2);
            /* data already in chunk+14 */

            udp_send(client_ip, KSERVE_PORT, KSERVE_PORT,
                     chunk, (UH)(14 + ulen));

            offset += (UW)n;

            /* Tiny yield so we don't starve other tasks */
            tk_dly_tsk(1);
        }

        vfs_close(fd);

        /* ---- KLRE ------------------------------------------------- */
        meta[0]='K'; meta[1]='L'; meta[2]='R'; meta[3]='E';
        __builtin_memcpy(meta+4, &session_id, 4);
        __builtin_memcpy(meta+8, &total,      4);
        udp_send(client_ip, KSERVE_PORT, KSERVE_PORT, meta, 12);

        tm_putstring((UB *)"[kserve] done\r\n");
    }
}

/* ------------------------------------------------------------------ */
/* Public init (called from usermain)                                 */
/* ------------------------------------------------------------------ */

void kserve_init(void)
{
    /* Create semaphore */
    T_CSEM csem = { .sematr=TA_TFIFO, .isemcnt=0, .maxsem=1 };
    kserve_sem = tk_cre_sem(&csem);
    if (kserve_sem < E_OK) return;

    /* Bind UDP port and join KLRQ multicast group (230.0.0.1) */
    if (udp_bind(KSERVE_PORT, kserve_recv) != 0) return;
    udp_join_group(KSERVE_PORT, IP4(230,0,0,1));

    /* Start serve task at low priority (below shell), ring-0 kernel task */
    T_CTSK ct = {
        .tskatr = TA_HLNG,
        .task   = kserve_task,
        .itskpri= 12,
        .stksz  = 4096,
    };
    ID tid = tk_cre_tsk(&ct);
    if (tid >= E_OK) tk_sta_tsk(tid, 0);

    tm_putstring((UB *)"[kserve] listening on UDP 7370\r\n");
}
