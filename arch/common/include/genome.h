/*
 *  genome.h — §3 self-regeneration: the swarm grows a new full cell.
 *
 *  Spec: docs/architecture/00-concept/survival-network.md §3 (装甲板＝細胞) and
 *  docs/architecture/genome.md.
 *
 *  Every organ this needs already exists; genome.c only ORCHESTRATES:
 *    weights  = p-fs versioned object "dtr/weights"  (dtr save / load)
 *    code     = p-fs versioned object "genome.c"     (selfc save / run)
 *    engrams  = p-fs versioned object "dtr/engrams"  (optional, wave 9-①)
 *    transport= p-fs P1 block gossip + P2 ref gossip (region-automatic)
 *
 *  A full cell runs `genome publish <role>`: it writes a small fixed-
 *  width MANIFEST that names those objects and saves it as the p-fs
 *  named ref "genome/manifest". The manifest is itself an ordinary
 *  content-addressed block, so P1/P2 replicate it like everything else.
 *
 *  An empty plate runs `genome sprout` (or boots with PKERNEL_SPROUT=1
 *  on hosted builds): it waits for "genome/manifest" to gossip in, then
 *  resolves each entry — weights through the dtr load core, code
 *  through in-kernel libtcc compilation (selfc), engrams by pulling the
 *  replica — and becomes a full cell.
 *
 *  HONEST LIMITS (see docs/architecture/30-module/genome.md §5):
 *    - the kernel IMAGE itself is not distributed (no bare-metal
 *      self-reflashing); code distribution means selfc C-source tasks
 *    - role is carried and displayed but nothing schedules by it yet
 *    - there is no signature / verification on the manifest or its
 *      entries — same trust model as selfc, stated, not solved
 *
 *  LP64 / wire discipline (feedback_lp64_typedef_trap): fixed-width
 *  fields only; exact sizes pinned by _Static_assert in genome.c.
 */
#pragma once
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* constants                                                           */
/* ------------------------------------------------------------------ */

#define GENOME_MAGIC      0x4D4F4E47UL  /* "GNOM" LE                    */
#define GENOME_VER        1
#define GENOME_ENTRY_MAX  4
#define GENOME_NAME_LEN   24            /* p-fs named-ref chars         */

/* the manifest's own p-fs named ref (15 chars <= PFS_NAME_MAX) */
#define GENOME_REF        "genome/manifest"
#define GENOME_REF_LEN    15

/* the named refs a manifest points at (today's conventions) */
#define GENOME_WEIGHTS_REF      "dtr/weights"
#define GENOME_WEIGHTS_REF_LEN  11
#define GENOME_CODE_REF         "genome.c"
#define GENOME_CODE_REF_LEN     8
#define GENOME_ENGRAMS_REF      "dtr/engrams"
#define GENOME_ENGRAMS_REF_LEN  11

/* entry kinds */
#define GENOME_K_WEIGHTS  1             /* dtr weight blob              */
#define GENOME_K_CODE     2             /* C source for selfc           */
#define GENOME_K_ENGRAMS  3             /* episodic memory (wave 9-①)   */

/* roles — carried + displayed; nothing schedules by role yet (honest) */
#define GENOME_ROLE_NONE    0
#define GENOME_ROLE_CELL    1
#define GENOME_ROLE_BRAIN   2
#define GENOME_ROLE_SENSOR  3
#define GENOME_ROLE_RELAY   4

/* ------------------------------------------------------------------ */
/* wire structs — fixed width, packed, sizes pinned in genome.c        */
/* ------------------------------------------------------------------ */

/* one genome entry: what kind of part, and which p-fs named ref */
typedef struct {
    U1   kind;                     /* GENOME_K_*                        */
    U1   name_len;                 /* valid chars in name[]             */
    UH   _pad;
    char name[GENOME_NAME_LEN];    /* p-fs named ref, NUL-padded        */
} __attribute__((packed)) GENOME_ENTRY;          /* 1+1+2+24 = 28 B    */

/* the manifest — the cell's DNA index. Saved (length-trimmed to the
 * used entries) as the p-fs object "genome/manifest". */
typedef struct {
    UW   magic;                    /* GENOME_MAGIC                      */
    UH   ver;                      /* GENOME_VER                        */
    U1   role;                     /* GENOME_ROLE_*                     */
    U1   reserved;                 /* 0                                 */
    UW   entry_cnt;                /* used entries in entries[]         */
    GENOME_ENTRY entries[GENOME_ENTRY_MAX];
} __attribute__((packed)) GENOME_MANIFEST;       /* 12 + 4*28 = 124 B  */

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* Publish this cell's genome: dtr/weights is MANDATORY (a genome
 * without a brain describes nothing); genome.c and dtr/engrams are
 * included only if those refs exist here. Saves the manifest as the
 * p-fs named ref "genome/manifest" (replicates region-wide).
 * Returns PFS_OK or a negative PFS_E_* / -1. Shell-task context only
 * (pfs_dag_read/save scratch discipline). */
INT  genome_publish(U1 role);

/* Germinate: wait up to `tries` x 500 ms for "genome/manifest" to
 * arrive via gossip (each probe also issues a P1 WANT), then resolve
 * every entry. tries==0 picks a default. Returns 0 once the cell has
 * sprouted, -1 if no manifest or no weights arrived. Shell-task
 * context only. */
INT  genome_sprout(UW tries);

/* Status: role / sprouted flag / local manifest contents. */
void genome_print(void);

/* Shell dispatcher for the full line "genome [publish <role>|sprout]". */
void genome_cmd(const UB *line, INT n);

/* Hosted only: if PKERNEL_SPROUT=1 in the environment, run an
 * auto-germination attempt (a long-budget genome_sprout). No-op on
 * bare metal and when the variable is unset — existing demos see no
 * behaviour change. Call from usermain after the autonet block. */
void genome_autosprout(void);

/* 1 if THIS node has published a genome manifest this boot. spawn.c
 * uses this to point a freshly spawned node at germination; it is a
 * local flag (not a p-fs probe) so it is safe to read from net-task
 * context — pfs_dag_read's scratch is shell-task-only. */
U1   genome_published_here(void);
