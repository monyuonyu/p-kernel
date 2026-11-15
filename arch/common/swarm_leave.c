/*
 *  swarm_leave.c — ROADMAP 0-14 S1: a node that leaves keeps nothing of the
 *  swarm (docs/architecture/30-module/swarm-leave.md, the design and the
 *  checks L1-L5 of tests/host/run_swarm_leave.sh, fixed before this code).
 *
 *  `leave` (console only): ① this node's local stop (S2 `stop now`) ② every
 *  p-fs block whose origin is not this node is removed (the table and the flat
 *  files; the origin of a file is its "<hex>.org", pfs_durable.c) ③ the
 *  weights files mind.rw and ns1_student.bin are removed (merged weights carry
 *  the swarm's share and cannot be split; the next boot pretrains as usual)
 *  ④ the mark swarm-left.st is written: until `join`, cmd_net stays off.
 *  The owner's own blocks, ark.key and node_seed are kept (global rule 11).
 *  A block without its .org (written before S1) is moved to left-unknown/,
 *  not deleted, and counted: the owner decides.
 *
 *  Hosted only (bare metal keeps nothing on disk). Limits: the ARK image's
 *  blocks carry no origin (the flat store only); the weights in RAM stay until
 *  the restart `leave` asks for (pfs_dur_freeze_weights: they are not written
 *  back meanwhile); copies of the owner's blocks on other nodes are not asked
 *  back.
 *  Negatives: -DS1_NC_LEAVE_KEEP_FOREIGN (L1), -DS1_NC_LEAVE_NO_FLAG (L3);
 *  -DS1_NC_ORIGIN_VOLATILE (L4) is in pfs_block.c.
 */

#include "swarm_leave.h"
#include "swarm_stop.h"
#include "pfs_block.h"
#include "drpc.h"

#ifdef _TK_HOSTED_LIBC_
extern int pfs_dur_active(void);
extern int pfs_dur_write(const char *fname, const void *data, unsigned len);
extern int pfs_dur_remove(const char *fname);
extern int pfs_dur_exists(const char *fname);
extern int pfs_dur_remove_block(const char *hex);
extern int pfs_dur_move_aside(const char *hex, const char *sub);
extern int pfs_dur_foreach_origin(void (*cb)(const char *hex, int origin, void *ctx), void *ctx);
extern void pfs_dur_freeze_weights(void);

#define SL_MARK    "swarm-left.st"
#define SL_UNKNOWN "left-unknown"

static INT sl_is_self(INT origin)
{
    return origin == (INT)PFS_ORIGIN_SELF || origin == (INT)drpc_my_node;
}

static void sl_dec(void (*out)(const char *), UW v)
{
    char b[12]; INT i = 11; b[11] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v && i > 0);
    out(&b[i]);
}

static INT sl_nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void sl_hex_to_id(const char *hex, U1 id[PFS_ID_LEN])
{
    INT i;
    for (i = 0; i < PFS_ID_LEN; i++)
        id[i] = (U1)((sl_nib(hex[2 * i]) << 4) | sl_nib(hex[2 * i + 1]));
}

typedef struct { UW self, foreign, unknown; INT act; } sl_count;

/* one flat block file: count it, and on `leave` remove the foreign one and
 * move the unknown one aside (and out of the table) */
static void sl_file(const char *hex, int origin, void *vc)
{
    sl_count *c = (sl_count *)vc;
    if (origin < 0) {
        c->unknown++;
        if (c->act && pfs_dur_move_aside(hex, SL_UNKNOWN) == 0) {
            U1 id[PFS_ID_LEN];
            sl_hex_to_id(hex, id);
            (void)pfs_drop(id);           /* the file is moved: this only empties the slot */
        }
    } else if (sl_is_self((INT)origin)) {
        c->self++;
    } else {
        c->foreign++;
#ifndef S1_NC_LEAVE_KEEP_FOREIGN
        if (c->act) (void)pfs_dur_remove_block(hex);
#endif
    }
}

void swarm_leave_origins(void (*out)(const char *))
{
    sl_count c = { 0, 0, 0, 0 };
    if (pfs_dur_active()) {
        (void)pfs_dur_foreach_origin(sl_file, &c);
    } else {                                  /* memory-only: the table */
        UW i; pk_node_t o;
        for (i = 0; i < PFS_MAX_BLOCKS; i++)
            if (pfs_slot_info(i, 0, 0, &o)) { if (sl_is_self((INT)o)) c.self++; else c.foreign++; }
    }
    out("[pfs] origins self "); sl_dec(out, c.self);
    out(" foreign "); sl_dec(out, c.foreign);
    out(" unknown "); sl_dec(out, c.unknown);
    out("\r\n");
}

INT swarm_leave_is_left(void)
{
    return pfs_dur_active() && pfs_dur_exists(SL_MARK) ? 1 : 0;
}

#ifdef SWARM_LEAVE_TEST
static void sl_id_to_hex(const U1 id[PFS_ID_LEN], char hex[2 * PFS_ID_LEN + 1])
{
    static const char H[] = "0123456789abcdef";
    INT i;
    for (i = 0; i < PFS_ID_LEN; i++) { hex[2 * i] = H[id[i] >> 4]; hex[2 * i + 1] = H[id[i] & 15]; }
    hex[2 * PFS_ID_LEN] = 0;
}

/* `leave seed self|<origin>`: one block with that origin (the test build only) */
static void sl_seed(const UB *a, UW n, void (*out)(const char *))
{
    static UW k = 0;
    UB buf[48];
    UW len = 0, i;
    INT self = 0, org = 0;
    U1 id[PFS_ID_LEN];
    char hex[2 * PFS_ID_LEN + 1];
    while (n && (*a == ' ' || *a == '\t')) { a++; n--; }
    if (n >= 4 && a[0] == 's' && a[1] == 'e' && a[2] == 'l' && a[3] == 'f') self = 1;
    else {
        for (i = 0; i < n && a[i] >= '0' && a[i] <= '9'; i++) org = org * 10 + (a[i] - '0');
        if (i == 0 || org < 1 || org > 65534) { out("usage: leave seed self|<origin 1..65534>\r\n"); return; }
    }
    {   /* "s1-seed <origin> <k>": a distinct block per call */
        const char *p = "s1-seed ";
        while (*p) buf[len++] = (UB)*p++;
        for (i = 0; i < n && i < 8; i++) buf[len++] = a[i];
        buf[len++] = ' ';
        buf[len++] = (UB)('a' + (k % 26)); buf[len++] = (UB)('a' + (k / 26) % 26);
        k++;
    }
    {
        INT rc = pfs_put_origin(buf, len, id, self ? PFS_ORIGIN_SELF : (pk_node_t)org);
        if (rc == PFS_E_LEFT) { out("[leave] seed: refused (this node left the swarm)\r\n"); return; }
        if (rc != PFS_OK) { out("[leave] seed: pfs_put failed\r\n"); return; }
    }
    sl_id_to_hex(id, hex);
    out("[leave] seed id="); out(hex);
    if (self) out(" origin=self\r\n");
    else { out(" origin="); sl_dec(out, (UW)org); out("\r\n"); }
}
#endif

/* remove what is not this node's: the table first (a foreign block there is
 * dropped with its file), then every file left (evicted foreign blocks;
 * unknown ones are moved aside). Returns the foreign count. */
static UW sl_sweep(sl_count *c)
{
    UW i, tab_foreign = 0;
    for (i = 0; i < PFS_MAX_BLOCKS; i++) {
        U1 id[PFS_ID_LEN]; UW len; pk_node_t o;
        if (!pfs_slot_info(i, id, &len, &o) || sl_is_self((INT)o)) continue;
        tab_foreign++;
#ifndef S1_NC_LEAVE_KEEP_FOREIGN
        (void)pfs_drop(id);
#endif
    }
    (void)pfs_dur_foreach_origin(sl_file, c);
    return tab_foreign + c->foreign;
}

void swarm_leave_cmd(const UB *a, UW n, void (*out)(const char *))
{
    sl_count c = { 0, 0, 0, 1 };
    UW weights = 0, foreign;
    while (n && (*a == ' ' || *a == '\t')) { a++; n--; }
#ifdef SWARM_LEAVE_TEST
    if (n >= 4 && a[0] == 's' && a[1] == 'e' && a[2] == 'e' && a[3] == 'd') { sl_seed(a + 4, n - 4, out); return; }
#endif
    if (n) { out("usage: leave   (this node leaves the swarm; `join` to come back)\r\n"); return; }
    if (!pfs_dur_active()) {
        out("[leave] no PKERNEL_PFS_DIR: nothing is kept on disk, nothing to remove\r\n");
        return;
    }
    swarm_stop_cmd((const UB *)" now", 4, out);          /* ① the local stop */
    /* audit-181: from here on nothing foreign is kept (the net's receive does
     * not obey the stop), and the mark goes down BEFORE the removal: a leave
     * that dies half way is finished by the sweep at the next start */
    pfs_set_refuse_foreign(1);
#ifndef S1_NC_LEAVE_NO_FLAG
    if (pfs_dur_write(SL_MARK, "PKLEFT1\n", 8) != 0) out("[leave] could not write " SL_MARK "\r\n");
#endif
    /* ② what is not this node's */
    foreign = sl_sweep(&c);
    /* ③ the weights */
    pfs_dur_freeze_weights();     /* and no write-back of the RAM copies until the restart */
    if (pfs_dur_exists("mind.rw"))         { (void)pfs_dur_remove("mind.rw"); weights++; }
    if (pfs_dur_exists("ns1_student.bin")) { (void)pfs_dur_remove("ns1_student.bin"); weights++; }
    out("[leave] foreign "); sl_dec(out, foreign);
    out(" weights "); sl_dec(out, weights);
    out(" unknown "); sl_dec(out, c.unknown);
    out(" kept "); sl_dec(out, c.self);
    out(" \r\n");
    if (c.unknown) out("[leave] blocks of unknown origin are in " SL_UNKNOWN "/ (not deleted)\r\n");
    out("[leave] restart to finish (the mind in RAM still has the swarm's weights); `join` to come back\r\n");
}

/* at start (after pfs_durable_restore, before the net): with the mark, keep
 * refusing foreign blocks and remove the ones on disk again (audit-181: what
 * arrived between `leave` and the restart, or a leave that died half way).
 * The weights files are not touched here: after `leave` a newborn's own
 * weights may be saved, and those are not the swarm's. */
void swarm_leave_restore(void (*out)(const char *))
{
    sl_count c = { 0, 0, 0, 1 };
    UW foreign;
    if (!swarm_leave_is_left()) return;
    pfs_set_refuse_foreign(1);
#ifndef S1_NC_LEFT_NO_SWEEP
    foreign = sl_sweep(&c);
#else
    foreign = 0;
#endif
    out("[leave] left at start: foreign "); sl_dec(out, foreign);
    out(" removed, unknown "); sl_dec(out, c.unknown);
    out(" moved aside\r\n");
}

void swarm_join_cmd(void (*out)(const char *))
{
    if (!swarm_leave_is_left()) { out("[leave] not left\r\n"); return; }
    if (pfs_dur_remove(SL_MARK) != 0) { out("[leave] could not remove " SL_MARK "\r\n"); return; }
    pfs_set_refuse_foreign(0);
    swarm_stop_cmd((const UB *)" resume", 7, out);
    out("[leave] joined: the network comes up at the next start\r\n");
}
#endif /* _TK_HOSTED_LIBC_ */
