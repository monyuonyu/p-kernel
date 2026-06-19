/*
 *  self_access.c — self-access R0: READ-ONLY introspection of the node's
 *  own "body".  See self_access.h + docs/architecture/self-access.md.
 *
 *  THE CLAIM (R0): the resident mind (or an operator) can READ what the node
 *  already knows about ITSELF — its tasks/processes, its p-fs/ark named
 *  objects (names + head versions, NOT contents), the devices/sensors
 *  present, and kernel self-stats (uptime, node id, alive peers) — and NOTHING
 *  in this module can WRITE, EXEC, register a driver, or open a file for
 *  writing.  Every getter copies live state out; none mutates it.
 *
 *  THE ONE SANCTIONED SIDE EFFECT (Q3=YES): an EXPLICIT body-touch
 *  (self_access_body) appends ONE event to the hash-chained "self/lin"
 *  autobiography via lm_self_append_introspect — the mind's self-examination
 *  becomes part of its honest history.  That append is the WHOLE of the
 *  module's write surface, and it writes a self-RECORD, never the body.
 *
 *  HONEST BOUND: R1+ (the mind WRITING / DRIVING its body, and the mind
 *  invoking introspection AUTONOMOUSLY) is Q1/Q2-gated and DEFERRED.  R0 only
 *  provides the capability + the `body` shell verb; a human invokes it.
 *
 *  arch/common discipline: no host libc, fixed-width types, sio_send_frame
 *  output, no large task-stack buffers.
 */

#include "self_access.h"
#include "drpc.h"          /* drpc_my_node, dnode_table[], DNODE_ALIVE      */
#include "dproc.h"         /* dproc_running_count (READ-ONLY)              */
#include "pfs_dag.h"       /* pfs_dag_foreach_ref (READ-ONLY)             */
#include "lm_self.h"       /* lm_self_append_introspect — the Q3 record    */
#include "moe.h"           /* MOE_NUM_CLASSES — the sensor band            */
#include "netstack.h"      /* net_my_ip — network presence                 */
#include "kernel.h"        /* SYSTIM / tk_get_otm                          */

/* ------------------------------------------------------------------ */
/* output helpers (sio frame channel; arch/common rule: no libc here)  */
/* ------------------------------------------------------------------ */

IMPORT void sio_send_frame(const UB *buf, INT size);

static void sp(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}
static void spd(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { sp("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    sp(&buf[i]);
}
/* dotted-quad print of net_my_ip (IP4 little-endian byte order). */
static void sp_ip(UW ip)
{
    spd(ip & 0xFF); sp(".");
    spd((ip >> 8) & 0xFF); sp(".");
    spd((ip >> 16) & 0xFF); sp(".");
    spd((ip >> 24) & 0xFF);
}

/* ------------------------------------------------------------------ */
/* read-only kernel/cluster self-stats                                 */
/* ------------------------------------------------------------------ */

static UW self_uptime_ms(void)
{
    SYSTIM t; tk_get_otm(&t);
    return (UW)t.lo;
}

/* count DNODE_ALIVE peers in the shared dnode_table[], EXCLUDING self.
 * READ-ONLY: only reads the .state field. */
static UW self_alive_peers(void)
{
    UW n = 0;
    for (INT i = 0; i < DNODE_MAX; i++) {
        if ((UB)i == drpc_my_node) continue;          /* exclude self */
        if (dnode_table[i].state == DNODE_ALIVE) n++;
    }
    return n;
}

void self_access_stats(SELF_STATS *out)
{
    if (!out) return;
    out->node_id        = drpc_my_node;
    out->uptime_ms      = self_uptime_ms();
    out->alive_peers    = self_alive_peers();
    out->running_procs  = dproc_running_count();
    out->pfs_objects    = self_access_pfs_count();
    out->sensor_classes = (UW)MOE_NUM_CLASSES;
    out->my_ip          = net_my_ip;
}

/* ------------------------------------------------------------------ */
/* read-only p-fs/ark object listing (names + head versions, NO bytes) */
/* ------------------------------------------------------------------ */

/* counter callback for pfs_dag_foreach_ref. */
static void pfs_count_cb(void *ctx, const char *name, UW seq, UB origin)
{
    (void)name; (void)seq; (void)origin;
    (*(UW *)ctx)++;
}

UW self_access_pfs_count(void)
{
    UW n = 0;
    (void)pfs_dag_foreach_ref(pfs_count_cb, &n);
    return n;
}

/* printer callback: one line per named object (name + head seq + origin). */
static void pfs_print_cb(void *ctx, const char *name, UW seq, UB origin)
{
    (void)ctx;
    sp("    "); sp(name);
    sp("  v"); spd(seq);
    sp("  origin="); spd((UW)origin);
    sp("\r\n");
}

/* ------------------------------------------------------------------ */
/* devices / sensors present (READ-ONLY presence checks)               */
/* ------------------------------------------------------------------ */

static void self_print_devices(void)
{
    sp("  devices / sensors:\r\n");
    /* the moe sensor band — the classes the brain perceives the world in. */
    sp("    sensor: moe gate, "); spd((UW)MOE_NUM_CLASSES);
    sp(" classes (present)\r\n");
    /* network interface presence: net_my_ip != 0 means the stack is up. */
    sp("    netif:  ");
    if (net_my_ip) { sp("up, ip="); sp_ip(net_my_ip); sp("\r\n"); }
    else           { sp("down (no ip)\r\n"); }
}

/* ------------------------------------------------------------------ */
/* the full introspection report (READ-ONLY; appends nothing)          */
/* ------------------------------------------------------------------ */

void self_access_print(void)
{
    SELF_STATS s;
    self_access_stats(&s);

    sp("[body] ==== self-access R0: READ-ONLY introspection ====\r\n");

    /* SELF_DOMAIN_STATS */
    sp("  stats:\r\n");
    sp("    node_id:      ");
    if (s.node_id == 0xFF) sp("(uninitialized)\r\n");
    else { spd((UW)s.node_id); sp("\r\n"); }
    sp("    uptime_ms:    "); spd(s.uptime_ms);     sp("\r\n");
    sp("    alive_peers:  "); spd(s.alive_peers);   sp(" (excl. self)\r\n");
    sp("    running_proc: "); spd(s.running_procs); sp("\r\n");
    sp("    pfs_objects:  "); spd(s.pfs_objects);   sp("\r\n");

    /* SELF_DOMAIN_TASKS — the cluster process list (dproc owns the print). */
    sp("  tasks / processes:\r\n");
    dproc_list();

    /* SELF_DOMAIN_FILES — named p-fs/ark objects (names + versions only). */
    sp("  p-fs / ark objects (names + head version; NOT contents):\r\n");
    {
        UW listed = pfs_dag_foreach_ref(pfs_print_cb, 0);
        if (listed == 0) sp("    (none)\r\n");
    }

    /* SELF_DOMAIN_DEVICES */
    self_print_devices();
}

/* ------------------------------------------------------------------ */
/* the `body` shell verb: print + Q3=YES Self-lineage append           */
/* ------------------------------------------------------------------ */

U4 self_access_body(void)
{
    self_access_print();

    /* Q3=YES: record this EXPLICIT body-touch onto the autobiography. The
     * append is the module's ONLY write, and it writes a self-RECORD of the
     * read (which domains were examined) — never the body itself. Bounded:
     * exactly ONE entry per invocation. */
    INT r = lm_self_append_introspect(SELF_DOMAIN_ALL);

    INT germ = 0, reap = 0, roll = 0, ok = 0;
    U4  head_seq = 0;
    if (r == PFS_OK) {
        /* read back the lineage head seq as proof the entry landed. */
        (void)lm_self_unit_lineage_check(&germ, &reap, &roll, &ok);
        head_seq = lm_self_head_seq();
        sp("[body] Q3 lineage: appended self-introspection event"
           " (kind=INTROSPECT); chain verifies=");
        sp(ok ? "yes" : "no");
        sp(", head seq="); spd((UW)head_seq);
        sp("\r\n");
    } else {
        sp("[body] Q3 lineage: append failed (report above still valid)\r\n");
    }
    return (r == PFS_OK) ? head_seq : 0;
}
