/*
 *  drpc.h (x86)
 *  Distributed T-Kernel RPC over UDP
 *
 *  Two QEMU instances share a virtual Ethernet cable (socket networking).
 *  The same bootloader.bin binary detects its node ID from the MAC address
 *  at boot time — no recompilation needed per node.
 *
 *    Terminal 0:  make run-node0   →  Node 0  IP=10.1.0.1  MAC=52:54:00:00:00:01
 *    Terminal 1:  make run-node1   →  Node 1  IP=10.1.0.2  MAC=52:54:00:00:00:02
 *
 *  Protocol (UDP port 7374):
 *    HEARTBEAT  500 ms broadcast → peer discovery + keep-alive
 *    REQ/REPLY  synchronous RPC (3 s timeout)
 *
 *  Distributed T-Kernel API:
 *    dtk_cre_tsk(node, func_id, pri)   create task on any node
 *    dtk_cre_sem(isemcnt)              create semaphore, returns global ID
 *    dtk_sig_sem(global_semid, cnt)    signal (routes over network if remote)
 *    dtk_wai_sem(global_semid, cnt, t) wait  (semaphore must live on local node)
 *
 *  Global Object ID encoding:
 *    bits 31..24 = node_id (0-7)
 *    bits 23.. 0 = local T-Kernel object ID
 */

#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define DRPC_PORT       7374        /* UDP port for all DRPC traffic     */
#define DNODE_MAX       8           /* max nodes in cluster (node 0-7)   */

/* ------------------------------------------------------------------ */
/* Packet header                                                        */
/* ------------------------------------------------------------------ */

#define DRPC_MAGIC      0x4B525064UL    /* "dRPK" LE                     */
#define DRPC_VERSION    1

/* type field values */
#define DRPC_HEARTBEAT  0x01
#define DRPC_REQ        0x10
#define DRPC_REPLY      0x11

/* call_id values */
#define DRPC_CALL_PING      0x0001
#define DRPC_CALL_CRE_TSK   0x0101    /* create task on remote node      */
#define DRPC_CALL_SIG_SEM   0x0201    /* signal remote semaphore         */
#define DRPC_CALL_INFER     0x0301    /* MLP inference on remote node    */
#define DRPC_CALL_FL_AGG    0x0401    /* FedAvg weight aggregation       */

typedef struct {
    UW  magic;          /* DRPC_MAGIC                        */
    UB  version;        /* DRPC_VERSION                      */
    UB  type;           /* DRPC_HEARTBEAT / REQ / REPLY      */
    UH  seq;            /* sequence number                   */
    UB  src_node;
    UB  dst_node;
    UH  call_id;        /* DRPC_CALL_*                       */
    UW  obj_id;         /* global object ID or func_id       */
    W   arg[3];         /* call arguments                    */
    W   result;         /* return value (REPLY only)         */
} __attribute__((packed)) DRPC_PKT;   /* 32 bytes */

/* ------------------------------------------------------------------ */
/* Global Object ID                                                    */
/* ------------------------------------------------------------------ */

#define GOBJ_MAKE(node, local)  \
    (((UW)(node) << 24) | ((UW)(local) & 0x00FFFFFFUL))
#define GOBJ_NODE(g)    ((UB)((UW)(g) >> 24))
#define GOBJ_LOCAL(g)   ((UW)(g) & 0x00FFFFFFUL)

/* ------------------------------------------------------------------ */
/* Node state machine                                                  */
/* ------------------------------------------------------------------ */

#define DNODE_UNKNOWN   0   /* never heard from                          */
#define DNODE_ALIVE     1   /* heartbeat received recently               */
#define DNODE_SUSPECT   2   /* missed SUSPECT_THRESH heartbeats          */
#define DNODE_DEAD      3   /* missed DEAD_THRESH more; pending cancelled */

/* ------------------------------------------------------------------ */
/* Node table                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    UB  node_id;
    UW  ip;
    UB  state;  /* DNODE_UNKNOWN / ALIVE / SUSPECT / DEAD               */
    UB  missed; /* consecutive missed heartbeat periods                  */
} DNODE;

extern DNODE dnode_table[DNODE_MAX];
extern UB    drpc_my_node;          /* 0xFF = not initialized            */

/* ------------------------------------------------------------------ */
/* Init & tasks                                                        */
/* ------------------------------------------------------------------ */

/* Call from usermain after rtl8139_init(), before netstack_start().
 * Sets net_my_ip and registers UDP port. */
void drpc_init(UB my_node_id, UW my_ip);

/* T-Kernel heartbeat task (pri 5) */
void drpc_task(INT stacd, void *exinf);

/* UDP receive callback — registered on DRPC_PORT */
void drpc_rx(UW src_ip, UH src_port, const UB *data, UH len);

/* Print node table to serial */
void drpc_nodes_list(void);

/* ------------------------------------------------------------------ */
/* Distributed T-Kernel API                                            */
/* ------------------------------------------------------------------ */

/* Create a task (local or remote).
 * func_id: 0x0001=hello  0x0002=counter  (see drpc.c rfunc_table)
 * Returns local task ID (local) or remote task ID (remote), or negative error. */
W  dtk_cre_tsk(UB node_id, UH func_id, INT pri);

/* Create semaphore on this node; returns global semaphore ID. */
UW dtk_cre_sem(INT isemcnt);

/* Wait on semaphore (semaphore must be on local node). */
ER dtk_wai_sem(UW gsemid, INT cnt, TMO tmout);

/* Signal semaphore — routes over the network if on remote node. */
ER dtk_sig_sem(UW gsemid, INT cnt);

/* Run MLP inference on a remote node (or local if node_id==drpc_my_node).
 * sensor_packed = SENSOR_PACK(temp_q8, hum_q8, press_q8, light_q8)
 * Returns E_OK and sets *class_out to 0/1/2. */
ER dtk_infer(UB node_id, W sensor_packed, UB *class_out, TMO tmout);

/*
 * heal.c から呼ぶ公開ラッパー。
 * DEAD ノードの代わりにローカルで rfunc タスクを起動する。
 */
W drpc_local_restart(UH func_id, INT pri, UB caller_node);
