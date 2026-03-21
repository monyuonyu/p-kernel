/*
 *  net_ssy.c (x86)
 *  Network subsystem — ssid=1
 *
 *  Responsibilities:
 *    - UDP socket pool: bind / send / recv / join_group / leave_group
 *    - TCP connection pool: connect / write / read / close
 *    - Per-socket owner-task tracking → auto-close on task exit
 *
 *  Registered with tk_def_ssy(NET_SSID, &net_dssy).
 *  syscall_dispatch() calls knl_svc_ientry(&pk, fncd) for 0x200-0x2FF;
 *  the kernel routes that to net_svchdr() here.
 *
 *  cleanupfn (net_cleanupfn) is called by knl_ssy_cleanup() when any
 *  task exits, so sockets are released even if the task forgot to close.
 */

#include "kernel.h"
#include "task.h"
#include "p_syscall.h"
#include "netstack.h"
#include "vfs.h"
#include "net_ssy.h"
#include <syscall.h>
#include <tmonitor.h>

/* ------------------------------------------------------------------ */
/* UDP socket pool                                                      */
/* ------------------------------------------------------------------ */

#define UDP_BIND_MAX    4
#define UDP_RECV_BUFSZ  512

typedef struct {
    UH   port;
    UB   in_use;
    ID   owner_tid;         /* task that called SYS_UDP_BIND          */
    ID   rx_sem;            /* semaphore: count = packets waiting      */
    UW   src_ip;
    UH   src_port;
    UH   data_len;
    UB   data[UDP_RECV_BUFSZ];
} USR_UDP_SLOT;

static USR_UDP_SLOT usr_udp[UDP_BIND_MAX];

/* One receive callback per slot (each captures its slot index) */
static void usr_udp_rx(INT slot, UW src_ip, UH src_port,
                        const UB *data, UH len)
{
    USR_UDP_SLOT *s = &usr_udp[slot];
    if (!s->in_use) return;
    if (len > UDP_RECV_BUFSZ) len = (UH)UDP_RECV_BUFSZ;
    s->src_ip   = src_ip;
    s->src_port = src_port;
    s->data_len = len;
    for (UH i = 0; i < len; i++) s->data[i] = data[i];
    tk_sig_sem(s->rx_sem, 1);
}

static void udp_rx0(UW i,UH p,const UB*d,UH l){usr_udp_rx(0,i,p,d,l);}
static void udp_rx1(UW i,UH p,const UB*d,UH l){usr_udp_rx(1,i,p,d,l);}
static void udp_rx2(UW i,UH p,const UB*d,UH l){usr_udp_rx(2,i,p,d,l);}
static void udp_rx3(UW i,UH p,const UB*d,UH l){usr_udp_rx(3,i,p,d,l);}

static const udp_recv_fn udp_cbs[UDP_BIND_MAX] = {
    udp_rx0, udp_rx1, udp_rx2, udp_rx3
};

/* ------------------------------------------------------------------ */
/* TCP connection pool                                                  */
/* ------------------------------------------------------------------ */

#define USR_TCP_MAX  4

typedef struct {
    TCP_CONN *conn;
    ID        owner_tid;    /* task that called SYS_TCP_CONNECT       */
} USR_TCP_SLOT;

static USR_TCP_SLOT usr_tcp[USR_TCP_MAX];

/* ------------------------------------------------------------------ */
/* cleanupfn — called by knl_ssy_cleanup() when a task exits          */
/* ------------------------------------------------------------------ */

static void net_cleanupfn(ID tskid)
{
    /* Close all UDP sockets owned by this task */
    for (INT i = 0; i < UDP_BIND_MAX; i++) {
        if (usr_udp[i].in_use && usr_udp[i].owner_tid == tskid) {
            udp_bind(usr_udp[i].port, NULL);    /* unregister callback */
            tk_del_sem(usr_udp[i].rx_sem);
            usr_udp[i].in_use = 0;
        }
    }
    /* Close all TCP connections owned by this task */
    for (INT i = 0; i < USR_TCP_MAX; i++) {
        if (usr_tcp[i].conn != NULL && usr_tcp[i].owner_tid == tskid) {
            tcp_close(usr_tcp[i].conn);
            tcp_free(usr_tcp[i].conn);
            usr_tcp[i].conn = NULL;
        }
    }
}

/* ------------------------------------------------------------------ */
/* svchdr dispatcher — handles 0x200..0x2FF                           */
/* ------------------------------------------------------------------ */

static W net_dispatch(W nr, W arg0, W arg1, W arg2)
{
    switch (nr) {

    /* --- UDP -------------------------------------------------------- */

    case SYS_UDP_BIND: {
        UH port = (UH)(UW)arg0;

        INT slot = -1;
        for (INT i = 0; i < UDP_BIND_MAX; i++) {
            if (!usr_udp[i].in_use) { slot = i; break; }
        }
        if (slot < 0) return -1;

        T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO,
                      .isemcnt = 0, .maxsem = 16 };
        ID sem = tk_cre_sem(&cs);
        if (sem < E_OK) return -1;

        if (udp_bind(port, udp_cbs[slot]) != 0) {
            tk_del_sem(sem);
            return -1;
        }

        usr_udp[slot].port      = port;
        usr_udp[slot].rx_sem    = sem;
        usr_udp[slot].data_len  = 0;
        usr_udp[slot].owner_tid = knl_ctxtsk->tskid;
        usr_udp[slot].in_use    = 1;
        return 0;
    }

    case SYS_UDP_SEND: {
        PK_UDP_SEND *pk = (PK_UDP_SEND *)(UW)arg0;
        if (!pk) return -1;
        return udp_send(pk->dst_ip, pk->src_port, pk->dst_port,
                        (const UB *)pk->buf_ptr, pk->len);
    }

    case SYS_UDP_RECV: {
        PK_UDP_RECV *pk = (PK_UDP_RECV *)(UW)arg0;
        if (!pk) return -1;

        INT slot = -1;
        for (INT i = 0; i < UDP_BIND_MAX; i++) {
            if (usr_udp[i].in_use && usr_udp[i].port == pk->port) {
                slot = i; break;
            }
        }
        if (slot < 0) return -1;

        ER er = tk_wai_sem(usr_udp[slot].rx_sem, 1, (TMO)pk->timeout_ms);
        if (er != E_OK) return (W)er;

        UH dlen = usr_udp[slot].data_len;
        if (dlen > pk->buflen) dlen = pk->buflen;
        UB *dst = (UB *)(UW)pk->buf_ptr;
        for (UH i = 0; i < dlen; i++) dst[i] = usr_udp[slot].data[i];

        pk->src_ip   = usr_udp[slot].src_ip;
        pk->src_port = usr_udp[slot].src_port;
        pk->data_len = dlen;
        return (W)dlen;
    }

    case SYS_UDP_JOIN_GROUP: {
        UH port     = (UH)(UW)arg0;
        UW mcast_ip = (UW)arg1;
        INT slot = -1;
        for (INT i = 0; i < UDP_BIND_MAX; i++) {
            if (usr_udp[i].in_use && usr_udp[i].port == port) {
                slot = i; break;
            }
        }
        if (slot < 0) return -1;
        return (W)udp_join_group(port, mcast_ip);
    }

    case SYS_UDP_LEAVE_GROUP: {
        UH port     = (UH)(UW)arg0;
        UW mcast_ip = (UW)arg1;
        return (W)udp_leave_group(port, mcast_ip);
    }

    /* --- TCP -------------------------------------------------------- */

    case SYS_TCP_CONNECT: {
        PK_TCP_CONNECT *pk = (PK_TCP_CONNECT *)(UW)arg0;
        if (!pk) return -1;

        INT slot = -1;
        for (INT i = 0; i < USR_TCP_MAX; i++) {
            if (!usr_tcp[i].conn) { slot = i; break; }
        }
        if (slot < 0) return -1;

        TCP_CONN *conn = NULL;
        if (tcp_connect(pk->dst_ip, pk->dst_port, &conn) < 0 || !conn)
            return -1;

        usr_tcp[slot].conn      = conn;
        usr_tcp[slot].owner_tid = knl_ctxtsk->tskid;
        return slot;
    }

    case SYS_TCP_WRITE: {
        INT h = (INT)arg0;
        if (h < 0 || h >= USR_TCP_MAX || !usr_tcp[h].conn) return -1;
        return tcp_write(usr_tcp[h].conn, (const UB *)(UW)arg1, (UH)arg2);
    }

    case SYS_TCP_READ: {
        PK_TCP_READ *pk = (PK_TCP_READ *)(UW)arg0;
        if (!pk) return -1;
        INT h = pk->handle;
        if (h < 0 || h >= USR_TCP_MAX || !usr_tcp[h].conn) return -1;
        return tcp_read(usr_tcp[h].conn, (UB *)(UW)pk->buf_ptr,
                        pk->buflen, pk->timeout_ms);
    }

    case SYS_TCP_CLOSE: {
        INT h = (INT)arg0;
        if (h < 0 || h >= USR_TCP_MAX || !usr_tcp[h].conn) return -1;
        tcp_close(usr_tcp[h].conn);
        tcp_free(usr_tcp[h].conn);
        usr_tcp[h].conn = NULL;
        return 0;
    }

    /* --- Mount (FS stub — will move to fs_ssy when implemented) ---- */

    case SYS_MOUNT:
        return vfs_ready ? 0 : -1;

    case SYS_UMOUNT:
        return 0;

    default:
        return -1;
    }
}

static INT net_svchdr(void *pk_para, FN fncd)
{
    (void)fncd;
    NET_SVC_PKT *pk = (NET_SVC_PKT *)pk_para;
    return (INT)net_dispatch(pk->nr, pk->arg0, pk->arg1, pk->arg2);
}

/* ------------------------------------------------------------------ */
/* net_ssy_init — called from usermain before tasks start             */
/* ------------------------------------------------------------------ */

void net_ssy_init(void)
{
    for (INT i = 0; i < UDP_BIND_MAX; i++) usr_udp[i].in_use = 0;
    for (INT i = 0; i < USR_TCP_MAX;  i++) {
        usr_tcp[i].conn      = NULL;
        usr_tcp[i].owner_tid = 0;
    }

    T_DSSY dssy;
    dssy.ssyatr    = TA_NULL;
    dssy.ssypri    = 1;
    dssy.svchdr    = (FP)net_svchdr;
    dssy.breakfn   = NULL;
    dssy.startupfn = NULL;
    dssy.cleanupfn = (FP)net_cleanupfn;
    dssy.eventfn   = NULL;
    dssy.resblksz  = 0;

    ER er = tk_def_ssy(NET_SSID, &dssy);
    if (er == E_OK)
        tm_putstring((UB *)"[net_ssy] registered (ssid=1)\r\n");
    else
        tm_putstring((UB *)"[net_ssy] tk_def_ssy FAILED\r\n");
}
