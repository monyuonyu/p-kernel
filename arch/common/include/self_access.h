/*
 *  self_access.h — self-access R0: READ-ONLY introspection of the node's
 *  own "body" (BACKLOG 🅰; mk_pino's "自分の体を触る" / the MCP-analogue
 *  embodiment, SAFE first slice).
 *
 *  Spec: docs/architecture/30-module/self-access.md.
 *
 *  R0 SCOPE — STRICTLY READ-ONLY. This module lets the resident mind (or an
 *  operator at the shell) INTROSPECT what the node already knows about
 *  itself: its task/process list, its p-fs/ark named objects (names + head
 *  versions, NEVER contents), the devices/sensors present, and kernel
 *  self-stats (uptime, node id, alive-peer count). It exposes NO write / exec
 *  / driver-register / fopen-for-write path. The ONE sanctioned side effect is
 *  the Q3=YES Self-lineage append (lm_self_append_introspect): each EXPLICIT
 *  body-touch is logged onto the hash-chained "self/lin" autobiography so the
 *  mind's self-examination becomes part of its honest history.
 *
 *  DEFERRED to R1+ (Q1/Q2-gated): any WRITE / EXEC / DRIVE path (the mind
 *  acting on its body), and the mind invoking introspection AUTONOMOUSLY. R0
 *  only provides the capability + the `body` shell verb; a human invokes it.
 *
 *  arch/common discipline: no host libc, fixed-width types, output via
 *  sio_send_frame, no task-stack large buffers.
 */

#pragma once
#include "kernel.h"

/* domains read by an introspection pass — a bitmask carried into the
 * Self-lineage event (records WHAT was read, never the read CONTENT). */
#define SELF_DOMAIN_STATS    0x1   /* uptime / node id / peers / mem      */
#define SELF_DOMAIN_TASKS    0x2   /* the cluster process list            */
#define SELF_DOMAIN_FILES    0x4   /* p-fs/ark named objects (names+seq)  */
#define SELF_DOMAIN_DEVICES  0x8   /* devices/sensors present             */
#define SELF_DOMAIN_ALL      (SELF_DOMAIN_STATS | SELF_DOMAIN_TASKS | \
                              SELF_DOMAIN_FILES | SELF_DOMAIN_DEVICES)

/* a flat read-only snapshot of the node's self-stats (no pointers, no
 * mutation; every field is copied out of live kernel/cluster state). */
typedef struct {
    UB  node_id;            /* drpc_my_node (0xFF = uninitialized)        */
    UW  uptime_ms;          /* coarse uptime (SYSTIM.lo)                  */
    UW  alive_peers;        /* DNODE_ALIVE entries in dnode_table (excl. self) */
    UW  running_procs;      /* cluster processes in the RUNNING state     */
    UW  pfs_objects;        /* named objects in the local p-fs ref table  */
    UW  sensor_classes;     /* MOE_NUM_CLASSES (the sensor band the brain reads) */
    UW  my_ip;             /* net_my_ip (0 = no network up)              */
} SELF_STATS;

/* Fill *out from live state. READ-ONLY — copies values, mutates nothing.
 * Returns nothing meaningful to mutate; the snapshot is in *out. */
void self_access_stats(SELF_STATS *out);

/* Count the node's named p-fs/ark objects (READ-ONLY; names+versions only,
 * never contents). Returns the count. */
UW   self_access_pfs_count(void);

/* Print a full human-readable introspection report over the sio frame
 * channel (the `body` shell verb drives this). READ-ONLY except for the
 * Q3=YES Self-lineage append below, which is performed by `self_access_body`
 * (this printer does NOT append, so it can be reused for testing). */
void self_access_print(void);

/* The shell verb entry point: print the introspection report AND append ONE
 * SELF_DOMAIN_ALL body-touch event to the Self-lineage (Q3=YES). Returns the
 * new lineage head seq (>=1) on a successful append, or 0 if the append
 * failed (the report still printed — introspection is never blocked by the
 * record). */
U4   self_access_body(void);

/* ---------------------------------------------------------------------
 * R1 — the first T1 (guarded state-changing) affordance: publish on the
 * node's OWN K-DDS topic (self-access-design.md §3.2/§7 R1; BACKLOG's
 * "self-access R1 + embodiment", mk_pino GREEN 2026-06-28).
 *
 * Confinement: this ALWAYS publishes to a topic name DERIVED from
 * drpc_my_node (never a caller-supplied topic string), so there is no
 * capability table to escape — the confinement is structural, not a
 * runtime check over an open namespace. (No general-purpose "germ
 * capability table" exists for parent-side/non-germ code as of
 * 2026-09-20 — confirmed by code search; that gap is why this affordance
 * is scoped to "own topic only" rather than an arbitrary p-fs write.)
 *
 * Refuses (E_OACV) while reflex_is_shielded() — same idiom usermain.c
 * already uses to refuse new selfc germination under an active shield
 * (arch/linux/{x86_64,aarch64}/usermain.c). Never touches
 * ark_consent_ok() — the human-consent boundary is not this affordance's
 * to cross.
 *
 * Returns E_OK on a successful publish, E_OACV while shielded, E_NOEXS if
 * the topic could not be opened (e.g. drpc_my_node uninitialized), or the
 * negative kdds_pub() error otherwise. */
INT  self_access_publish(const void *data, UW len);

/* [self-act-guarded] matched-arm self-test for R1's publish affordance
 * (self-access-design.md's gate: a T1 write succeeds only when NOT
 * shielded, and is refused while reflex_is_shielded()). Uses
 * reflex_test_force_shield() to drive both arms deterministically —
 * no real threat needs to fire. Prints one greppable PASS/FAIL line per
 * arm plus a summary. Returns 0 on PASS (both arms behaved), else a
 * fail count. Safe to call repeatedly; restores the shield to cleared
 * on exit either way. */
INT  self_access_r1_self_test(void);
