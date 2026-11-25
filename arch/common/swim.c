/*
 *  swim.c (x86)
 *  SWIM membership protocol — indirect probing + gossip dissemination
 *
 *  Uses the same dnode_table[] as drpc.c (shared extern).
 *  drpc_task continues broadcasting DRPC_HEARTBEAT for initial discovery;
 *  swim_task adds explicit PING/ACK probing on top for faster failure
 *  detection with indirect verification (avoids false positives).
 *
 *  Gossip TTL: each membership event is piggybacked GOSSIP_TTL times,
 *  giving O(log N) epidemic spread across the cluster.
 */
#define DRPC_TABLE_OWNER 1      /* N3 2a: one of the two owners of dnode_table */
#include "swim.h"
#include "pk_nosw.h"   /* gq[] is shared by swim_task and swim_rx (net_task) */
#include "heal.h"
#include "replica.h"
#include "degrade.h"
#include "dmn.h"
#include "galaxy.h"     /* galaxy v1: S1 membership emit hook */
#include "netstack.h"
#include "region.h"     /* N-2b: region_{set,is}_super_capable, region_supernode */
#if defined(_TK_HOSTED_LIBC_)
#include "nodemap.h"    /* N2b: wide ids */
#endif
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

static void sw_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void sw_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { sw_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    sw_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* Test-only patience (inbox #3-4, 2026-09-27)                         */
/*                                                                     */
/* A multi-node cert whose nodes block for tens of seconds (GGUF       */
/* generation, distillation) gets its peers declared DEAD mid-test.    */
/* HOSTED builds read PKERNEL_SWIM_SUSPECT_ROUNDS / _DEAD_ROUNDS       */
/* (1..250: suspect_count is a UB) once at swim_init; the product      */
/* defaults in swim.h are unchanged. Bare metal has no env:            */
/* SWIM_*_LIMIT stay the swim.h                                        */
/* constants, so its code is unchanged (crown-neutral).                */
/* Cert: tests/host/run_swim_env_rounds.sh.                            */
/* ------------------------------------------------------------------ */
#ifdef _TK_HOSTED_LIBC_
static INT swim_suspect_limit = SWIM_SUSPECT_ROUNDS;
static INT swim_dead_limit    = SWIM_DEAD_ROUNDS;
#define SWIM_SUSPECT_LIMIT swim_suspect_limit
#define SWIM_DEAD_LIMIT    swim_dead_limit

static INT swim_env_rounds(const char *name, INT dflt)
{
    extern char *getenv(const char *);
    const char *s = getenv(name);
    INT v = 0;
    if (!s || !*s) return dflt;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 250) return dflt;
        v = v * 10 + (*s - '0');
    }
    return (v >= 1 && v <= 250) ? v : dflt;
}

static void swim_rounds_from_env(void)
{
    swim_suspect_limit = swim_env_rounds("PKERNEL_SWIM_SUSPECT_ROUNDS", SWIM_SUSPECT_ROUNDS);
    swim_dead_limit    = swim_env_rounds("PKERNEL_SWIM_DEAD_ROUNDS",    SWIM_DEAD_ROUNDS);
    if (swim_suspect_limit != SWIM_SUSPECT_ROUNDS || swim_dead_limit != SWIM_DEAD_ROUNDS) {
        sw_puts("[swim] rounds suspect="); sw_putdec((UW)swim_suspect_limit);
        sw_puts(" dead=");                 sw_putdec((UW)swim_dead_limit);
        sw_puts(" (env)\r\n");
    }
}
#else
#define SWIM_SUSPECT_LIMIT SWIM_SUSPECT_ROUNDS
#define SWIM_DEAD_LIMIT    SWIM_DEAD_ROUNDS
#endif

/* ------------------------------------------------------------------ */
/* Gossip queue                                                        */
/*                                                                     */
/* Each entry is piggybacked on outgoing SWIM packets until            */
/* remaining == 0, then retired.  GOSSIP_TTL ≈ 2*log(N) = 6.         */
/* ------------------------------------------------------------------ */

#define GOSSIP_TTL    6
#define GOSSIP_Q_MAX  (SWIM_GOSSIP_MAX * 2)

typedef struct {
    pk_node_t node_id;
    UB  state;
    UB  incarnation;
    UB  remaining;
    UB  capability;   /* N-2b: rides the entry through the TTL piggyback */
} GOSSIP_QUEUED;

static GOSSIP_QUEUED gq[GOSSIP_Q_MAX];
static INT           gq_cnt = 0;

/* ------------------------------------------------------------------ */
/* Incarnation numbers (canonical SWIM anti-stale-rumor)               */
/*                                                                     */
/* SWIM-INCARN (external audit 2026-06-13): the `incarnation` field was */
/* on the wire but never assigned a real value and never compared, so   */
/* a node could not refute an OLD DEAD/SUSPECT rumor about itself with a */
/* FRESHER incarnation — the rumor's TTL could re-kill it.              */
/*                                                                     */
/* Fix: each node keeps a monotonic `my_incarnation`, bumped whenever it */
/* must refute a SUSPECT/DEAD rumor about ITSELF (re-asserting ALIVE at  */
/* a strictly higher incarnation). `dnode_incarn[nid]` mirrors the last  */
/* incarnation we accepted for each peer, and gossip_apply does a        */
/* (incarnation, state-severity) lexicographic last-writer-wins:         */
/*   - a strictly HIGHER incarnation always wins (regardless of state);  */
/*   - within the SAME incarnation, more-dead wins (ALIVE<SUSPECT<DEAD); */
/*   - a strictly LOWER incarnation is ignored (the stale rumor).        */
/* incarnation is a UB on the wire (swim.h) so it wraps at 256; bumps    */
/* are a plain ++ and the compare is on equality + a single-step ladder, */
/* so a wrap only loses one anti-stale round (self re-bumps next time it  */
/* is rumored) — sane and never UB. */
static UB my_incarnation        = 0;
#if defined(_TK_HOSTED_LIBC_)
/* N2c stage 2 (audit-200): > 0 while a swim self-test drives the refute
 * path with a pretend incarnation. The refute then must not reach
 * pk_node_incarn_note: the test restores my_incarnation, but a write to
 * node.incarn would stay (nodes test 3 -> 258). Bare: no note, no counter. */
static INT sw_selftest_depth = 0;
#define SW_SELFTEST_ENTER() (sw_selftest_depth++)
#define SW_SELFTEST_LEAVE() (sw_selftest_depth--)
#else
#define SW_SELFTEST_ENTER() ((void)0)
#define SW_SELFTEST_LEAVE() ((void)0)
#endif
#if defined(_TK_HOSTED_LIBC_)
/* ------------------------------------------------------------------ */
/* N3 step 1 (research/n3-swim-region.md §2): the per-peer SWIM state  */
/* is one SWIM_PEER per SLOT, not per node id. A narrow id takes a     */
/* slot (sw_map, a NODEMAP) the first time SWIM itself hears of it     */
/* (its own packet, or a rumour); when the slots are full a DEAD or    */
/* UNKNOWN holder is handed over, else the id is refused (counted in   */
/* sw_refused) and SWIM drops what it heard: the peer stays in the     */
/* drpc table (step 2 of N3) but out of SWIM's failure detection.      */
/* The seven tables that grew with DNODE_MAX (dnode_incarn,            */
/* suspect_count, v2cap_tab, rtt_valid, rtt_ewma_ms, swim_nping_seq,   */
/* swim_nping_ms) are the fields below. SWIM_PEERS is DREGION_MAX by   */
/* default (64 = every narrow id: nothing is ever refused there).      */
/* Bare metal keeps the id-indexed arrays (crown): the SW_* macros     */
/* below expand to them there.                                         */
/* -DN3_NC_NO_CAP: negative control of [n3-swim-cap] C1 (the slots are */
/* not capped at SWIM_PEERS: a node keeps every peer it hears of).     */
/* ------------------------------------------------------------------ */
#ifndef SWIM_PEERS
#define SWIM_PEERS DREGION_MAX
#endif
#if SWIM_PEERS < 1 || SWIM_PEERS > DREGION_MAX
#error "SWIM_PEERS must be 1..DREGION_MAX (the NODEMAP holds DREGION_MAX)"
#endif
#if defined(N3_NC_NO_CAP)
#define SW_STORE DREGION_MAX
#else
#define SW_STORE SWIM_PEERS
#endif
typedef struct {
    UW rtt_ewma_ms;   /* 平滑化済み RTT (ms), valid iff rtt_valid          */
    UW nping_ms;      /* when the v2 PING to it went out (a wide self)    */
    UH nping_seq;     /* seq of that v2 PING in flight, 0 = none          */
    UB incarn;        /* the last incarnation accepted for it             */
    UB suspect;       /* consecutive no-response rounds                   */
    UB v2cap;         /* capability bit 2: it reads v2                    */
    UB rtt_valid;     /* 1 = measured at least once                       */
#if defined(DNODE_ROWS_IN_SLOTS)
    DNODE row;        /* N3 2b: drpc's row of this id (drpc.h DROW/DROWW) */
#endif
} SWIM_PEER;
static SWIM_PEER sw_peer[SW_STORE];
/* narrow id -> slot in sw_peer. The cap is set here, not only in swim_init:
 * a node with no network never runs swim_init, but `nodes test` and the other
 * self-tests still drive gossip_apply (a zero cap refused every id). */
#if defined(N3_NC_NO_CAP)
static NODEMAP   sw_map = { .cap = DREGION_MAX };
#else
static NODEMAP   sw_map = { .cap = SWIM_PEERS };
#endif
static UW        sw_refused;        /* times an id found the slots full */
static SWIM_PEER sw_scratch;        /* self-tests only: a refused id    */

/* The graves (11-21, S5 C9): a peer whose slot is handed over leaves its id
 * and incarnation here (SW_STORE of them, the oldest overwritten). Measured
 * before: a killed peer restarted with a newer incarnation stayed DEAD on 9 of
 * 11 nodes (full slots dropped its word; master: 11/11 ALIVE again). Now its
 * slot comes back with the old incarnation, so only a NEWER word revives it
 * (and a stale ALIVE cannot: limit 1). -DN3_NC_NO_GRAVE: not kept (C9 red). */
/* N3 2b: st = the row state it had (a DEAD id stays DEAD to drpc after its
 * slot is handed over: a heartbeat or a stale ALIVE must not bring it back) */
typedef struct { pk_node_t id; UB incarn; UB used; UB st; } SW_GRAVE;
static SW_GRAVE sw_grave[SW_STORE];
static INT      sw_grave_next;
#if defined(DNODE_ROWS_IN_SLOTS)
#define SW_ROW_ST(i) (sw_peer[(i)].row.state)
#else
#define SW_ROW_ST(i) ((UB)DNODE_UNKNOWN)   /* step 1: the row is in dnode_table, never lost */
#endif

static void sw_grave_put(pk_node_t id, UB incarn, UB st)
{
#if defined(N3_NC_NO_GRAVE)
    (void)id; (void)incarn; (void)st;
#else
    INT g = -1;
    for (INT i = 0; i < SW_STORE && g < 0; i++) if (sw_grave[i].used && sw_grave[i].id == id) g = i;
    if (g < 0) { g = sw_grave_next; sw_grave_next = (sw_grave_next + 1) % SW_STORE; }
    sw_grave[g].id = id; sw_grave[g].incarn = incarn; sw_grave[g].used = 1; sw_grave[g].st = st;
#endif
}

/* the grave of id: its slot in sw_grave, or -1 */
static INT sw_grave_find(pk_node_t id)
{
    for (INT i = 0; i < SW_STORE; i++) if (sw_grave[i].used && sw_grave[i].id == id) return i;
    return -1;
}

/* is incarnation inc newer (serial order, as gossip_supersedes) than id's grave? */
static BOOL sw_grave_newer(pk_node_t id, UB inc)
{
    INT g = sw_grave_find(id);
    if (g < 0) return FALSE;
    UB d = (UB)(inc - sw_grave[g].incarn);
    return (d >= 1 && d <= 127) ? TRUE : FALSE;
}

/* the slot of narrow id n, or NULL (looks only) */
static SWIM_PEER *sw_get(pk_node_t n)
{
    if (n >= DNODE_MAX) return NULL;
    INT s = nodemap_find(&sw_map, n);
    return (s < 0 || s >= SW_STORE) ? NULL : &sw_peer[s];
}

/* How much THIS node wants peer n in a slot (limit 4 of step 1, 11-21): a
 * hash of (my id, n), so each node prefers its own set. First come kept the
 * same peers everywhere and the same few were in no node's slots (S5 C7). */
static UW sw_score(pk_node_t n)
{
    UW h = (UW)drpc_my_node * 0x9E3779B1UL ^ ((UW)n + 1u) * 0x85EBCA6BUL;
    h ^= h >> 16; h *= 0x7FEB352DUL; h ^= h >> 15; h *= 0x846CA68BUL; h ^= h >> 16;
    return h;
}

/* the slot of narrow id n, taken (zeroed) if new; NULL if refused.
 * may_displace: n may push out a live holder it outscores. Never for a dead n:
 * measured 11-21 (S5 C8, 555eec3a) a killed id's DEAD rumours re-took a slot
 * from a live holder each time it was handed over (3/9 nodes still held it). */
static SWIM_PEER *sw_take_ex(pk_node_t n, BOOL may_displace)
{
    (void)may_displace;   /* unused with -DN3_NC_FIRST_COME */
    if (n >= DNODE_MAX) return NULL;
    PK_NOSW_ENTER();      /* swim_rx and the self-tests take; swim_task reads */
    INT s = nodemap_find(&sw_map, n);
    if (s < 0) {
        s = nodemap_admit(&sw_map, n);
        for (INT i = 0; s < 0 && i < SW_STORE; i++) {
            if (!sw_map.used[i]) continue;
#if defined(DNODE_ROWS_IN_SLOTS)
            UB st = sw_peer[i].row.state;      /* N3 2b: the holder's own row */
#else
            UB st = DROW(sw_map.id[i]).state;
#endif
#if defined(N3_NC_NO_HANDOVER)
            st = DNODE_ALIVE;   /* negative control of [n3-swim-cap] C8 */
#endif
            if (st == DNODE_DEAD || st == DNODE_UNKNOWN) {   /* as wide_add */
                sw_grave_put((pk_node_t)sw_map.id[i], sw_peer[i].incarn, st);
                nodemap_evict(&sw_map, i);
                s = nodemap_admit(&sw_map, n);
            }
        }
#if !defined(N3_NC_FIRST_COME)
        /* still full: n replaces the holder this node wants least, if it wants
         * n more (stable once the best SWIM_PEERS are in; that holder is then
         * refused like any other). -DN3_NC_FIRST_COME: the step-1 rule (C7 red) */
#if defined(N3_NC_DEAD_DISPLACES)
        may_displace = TRUE;   /* negative control: the bug above (C8 red) */
#endif
        if (s < 0 && may_displace) {
            INT lo = -1; UW lo_sc = 0;
            for (INT i = 0; i < SW_STORE; i++) {
                if (!sw_map.used[i] || (pk_node_t)sw_map.id[i] == drpc_my_node) continue;
                UW sc = sw_score((pk_node_t)sw_map.id[i]);
                if (lo < 0 || sc < lo_sc) { lo = i; lo_sc = sc; }
            }
            if (lo >= 0 && sw_score(n) > lo_sc) {
                sw_grave_put((pk_node_t)sw_map.id[lo], sw_peer[lo].incarn, SW_ROW_ST(lo));
                nodemap_evict(&sw_map, lo);
                s = nodemap_admit(&sw_map, n);
            }
        }
#endif
        if (s >= SW_STORE) { nodemap_evict(&sw_map, s); s = -1; }   /* never: lowest free first */
        if (s < 0) { sw_refused++; PK_NOSW_LEAVE(); return NULL; }
        sw_peer[s] = (SWIM_PEER){ 0 };
#if defined(DNODE_ROWS_IN_SLOTS)
        sw_peer[s].row.node_id = n;    /* UNKNOWN, ip 0: as drpc_init left it */
#endif
        INT g = sw_grave_find(n);     /* back from a grave: the old incarnation */
        if (g >= 0) {
            sw_peer[s].incarn = sw_grave[g].incarn;
#if defined(DNODE_ROWS_IN_SLOTS)
            if (sw_grave[g].st == DNODE_DEAD) sw_peer[s].row.state = DNODE_DEAD;   /* still DEAD */
#endif
            sw_grave[g].used = 0;
        }
    }
    PK_NOSW_LEAVE();
    return &sw_peer[s];
}

static SWIM_PEER *sw_take(pk_node_t n)
{
    if (n >= DNODE_MAX) return NULL;
    return sw_take_ex(n, DROW(n).state != DNODE_DEAD ? TRUE : FALSE);
}

#if defined(DNODE_ROWS_IN_SLOTS)
/* N3 2b (research/n3-dnode-region.md §2.2): drpc's rows live in the slots.
 * rd: the row of n, or a zeroed row (UNKNOWN, ip 0) if n has no slot — a
 * write through it is lost and the next call zeroes it again. wr: the row,
 * taken like any SWIM word (full: a DEAD/UNKNOWN holder is handed over, or n
 * outscores the weakest live one, else refused and counted in sw_refused);
 * refused = a sink nobody reads. A slot handed over takes its row with it:
 * the old id reads UNKNOWN to drpc from then on. */
/* The node's own row is not in a slot: it always exists, as in the table.
 * Measured first with self in the slots (11-24, /build/n3b/cap): every node
 * lost its own row (a self-test's UNKNOWN moment handed the slot over, the
 * restore was refused) and read itself UNKNOWN. Keyed by the id drpc_init
 * registered, not by drpc_my_node (self-tests change that for a while). */
static DNODE     dnode_rd_zero, dnode_wr_sink, dnode_self_row;
static pk_node_t dnode_self_id;
static UB        dnode_self_set;
DNODE *dnode_row_rd(pk_node_t n)
{
    if (dnode_self_set && n == dnode_self_id) return &dnode_self_row;
    SWIM_PEER *p = sw_get(n);
    if (p) return &p->row;
    dnode_rd_zero = (DNODE){ .node_id = n };
    INT g = sw_grave_find(n);      /* handed over while DEAD: still DEAD */
    if (g >= 0 && sw_grave[g].st == DNODE_DEAD) dnode_rd_zero.state = DNODE_DEAD;
    return &dnode_rd_zero;
}
DNODE *dnode_row_wr(pk_node_t n)
{
    if (dnode_self_set && n == dnode_self_id) return &dnode_self_row;
    SWIM_PEER *p = sw_take(n);
    if (p) return &p->row;
    dnode_wr_sink = (DNODE){ .node_id = n };
    return &dnode_wr_sink;
}
/* drpc_init (after drpc_my_node is set): every row that exists back to
 * UNKNOWN (no slot is taken); a narrow self gets its own row (UNKNOWN until
 * drpc_init registers it) and gives back a slot it may hold. */
void dnode_rows_reset(void)
{
    for (INT i = 0; i < SW_STORE; i++)
        sw_peer[i].row = (DNODE){ .node_id = (pk_node_t)(sw_map.used[i] ? sw_map.id[i] : 0) };
    dnode_self_set = 0;
    if (drpc_my_node < DNODE_MAX) {     /* PK_NODE_IN_TABLE on hosted (pk_node_row.h) */
        INT s = nodemap_find(&sw_map, drpc_my_node);
        if (s >= 0) nodemap_evict(&sw_map, s);
        dnode_self_id  = drpc_my_node;
        dnode_self_row = (DNODE){ .node_id = drpc_my_node };
        dnode_self_set = 1;
    }
}

/* `nodes ownrow` (audit-227 m2, the hole C1..C11 did not see): the node's own
 * row is outside the slots. Pigeonhole on emptied slots: SW_STORE other narrow
 * ids and this node all write ALIVE twice. Own row outside: every other id keeps
 * a slot and this node reads ALIVE. Own row in a slot (m2: dnode_self_set = 0):
 * SW_STORE + 1 writers for SW_STORE slots, so one of them is refused or pushed
 * out = FAIL. Needs SW_STORE + 1 <= DNODE_MAX (a capped build, e.g. SWIM_PEERS=8
 * in run_n3_swim_cap.sh); else SKIP. Every slot, grave and the own row are
 * saved and put back; no task switch inside (the live SWIM sees nothing).
 * The save buffers are `used`: at DNODE_MAX 64 (= SW_STORE, the SKIP is a
 * constant) the compiler dropped them, at 256/1024 it kept them, and
 * [unbounded-swim] S1 read that as static data growing with the fleet (CI
 * 45456833357; their size is SW_STORE's, never DNODE_MAX's). */
static SWIM_PEER sw_or_peer[SW_STORE] __attribute__((used));
static SW_GRAVE  sw_or_grave[SW_STORE] __attribute__((used));
static NODEMAP   sw_or_map __attribute__((used));
/* C12 limit of audit-231 (a restore that misses a part was not seen by C1..C12:
 * live SWIM put it right again): the saved state is copied byte by byte and a
 * hash of it before the save must equal the hash after the put back.
 * -DN3_NC_OWNROW_NO_MAP: negative control (sw_map is not put back = C12 red). */
static void sw_or_cpy(void *d, const void *s, UW n)
{
    UB *dp = (UB *)d; const UB *sp = (const UB *)s;
    for (UW i = 0; i < n; i++) dp[i] = sp[i];
}
static UW sw_or_fnv(UW h, const void *p, UW n)
{
    const UB *b = (const UB *)p;
    for (UW i = 0; i < n; i++) { h ^= b[i]; h *= 16777619UL; }
    return h;
}
static UW sw_or_hash(void)
{
    UW h = 2166136261UL;
    h = sw_or_fnv(h, &sw_map, sizeof sw_map);
    h = sw_or_fnv(h, sw_peer, sizeof sw_peer);
    h = sw_or_fnv(h, sw_grave, sizeof sw_grave);
    h = sw_or_fnv(h, &sw_grave_next, sizeof sw_grave_next);
    h = sw_or_fnv(h, &sw_refused, sizeof sw_refused);
    h = sw_or_fnv(h, &dnode_self_row, sizeof dnode_self_row);
    h = sw_or_fnv(h, &dnode_self_set, sizeof dnode_self_set);
    return h;
}
INT swim_ownrow_self_test(void (*emit)(const char *))
{
    void (*say)(const char *) = emit ? emit : sw_puts;
    pk_node_t me = drpc_my_node;
    if (me >= DNODE_MAX || SW_STORE + 1 > DNODE_MAX) {
        say("[ownrow] SKIP (no narrow id, or the slots hold every narrow id)\r\n");
        return 0;
    }
    SW_SELFTEST_ENTER();
    PK_NOSW_ENTER();
    UW h0 = sw_or_hash();
    sw_or_cpy(&sw_or_map, &sw_map, sizeof sw_map);
    sw_or_cpy(sw_or_peer, sw_peer, sizeof sw_peer);
    sw_or_cpy(sw_or_grave, sw_grave, sizeof sw_grave);
    INT gnext = sw_grave_next; UW refused = sw_refused;
    DNODE own; sw_or_cpy(&own, &dnode_self_row, sizeof own);
    UB own_set = dnode_self_set;
    UH cap = sw_map.cap;
    nodemap_init(&sw_map, cap);
    for (INT i = 0; i < SW_STORE; i++) sw_grave[i].used = 0;
    INT fails = 0, kept = 0;
    for (INT pass = 0; pass < 2; pass++) {
        INT k = 0;
        for (pk_node_t n = 0; n < DNODE_MAX && k < SW_STORE; n++) {
            if (n == me) continue;
            DROWW(n).state = DNODE_ALIVE; k++;
        }
        DROWW(me).state = DNODE_ALIVE;
    }
    {
        INT k = 0;
        for (pk_node_t n = 0; n < DNODE_MAX && k < SW_STORE; n++) {
            if (n == me) continue;
            if (sw_get(n) && DROW(n).state == DNODE_ALIVE) kept++;
            k++;
        }
    }
    UB mine = DROW(me).state;
    if (kept != SW_STORE) fails++;
    if (mine != DNODE_ALIVE) fails++;
    /* put everything back */
#if !defined(N3_NC_OWNROW_NO_MAP)
    sw_or_cpy(&sw_map, &sw_or_map, sizeof sw_map);
#endif
    sw_or_cpy(sw_peer, sw_or_peer, sizeof sw_peer);
    sw_or_cpy(sw_grave, sw_or_grave, sizeof sw_grave);
    sw_grave_next = gnext; sw_refused = refused;
    sw_or_cpy(&dnode_self_row, &own, sizeof own); dnode_self_set = own_set;
    UW h1 = sw_or_hash();
    if (h1 != h0) fails++;
    PK_NOSW_LEAVE();
    SW_SELFTEST_LEAVE();
    say("[ownrow] slots "); sw_putdec((UW)SW_STORE); say(", other ids that kept a slot ");
    sw_putdec((UW)kept); say(", own row "); say(mine == DNODE_ALIVE ? "ALIVE" : "not ALIVE");
    say(h1 == h0 ? ", put back whole" : ", put back NOT whole (the state differs)"); say("\r\n");
    say(fails ? (h1 != h0 ? "[ownrow] FAIL (the state was not put back)\r\n"
                          : "[ownrow] FAIL (the own row competes for a slot)\r\n")
              : "[ownrow] PASS (the own row is outside the slots, the state put back)\r\n");
    return fails;
}
#endif

static UB   sw_inc_get(pk_node_t n)
{
    SWIM_PEER *p = sw_get(n);
    if (p) return p->incarn;
    INT g = sw_grave_find(n);        /* e.g. rx telling a dead sender DEAD@k */
    return g >= 0 ? sw_grave[g].incarn : 0;
}
static void sw_inc_set(pk_node_t n, UB v)  { SWIM_PEER *p = sw_get(n); if (p) p->incarn = v; }
static UB   sw_sus_get(pk_node_t n)        { SWIM_PEER *p = sw_get(n); return p ? p->suspect : 0; }
static void sw_sus_set(pk_node_t n, UB v)  { SWIM_PEER *p = sw_get(n); if (p) p->suspect = v; }
static void sw_sus_inc(pk_node_t n)        { SWIM_PEER *p = sw_get(n); if (p) p->suspect++; }
/* self-tests drive crafted state into a peer: take its slot (or a scratch) */
static SWIM_PEER *sw_tslot(pk_node_t n)
{
    SWIM_PEER *p = sw_take(n);
    if (p) return p;
    sw_scratch = (SWIM_PEER){ 0 };
    return &sw_scratch;
}
#define SW_INC(n)         sw_inc_get(n)
#define SW_INC_SET(n, v)  sw_inc_set((n), (v))
#define SW_SUS(n)         sw_sus_get(n)
#define SW_SUS_SET(n, v)  sw_sus_set((n), (v))
#define SW_SUS_INC(n)     sw_sus_inc(n)
#define SWT_INC(n)        (sw_tslot(n)->incarn)
#define SWT_SUS(n)        (sw_tslot(n)->suspect)
#define SWT_RTTV(n)       (sw_tslot(n)->rtt_valid)
#define SWT_RTT(n)        (sw_tslot(n)->rtt_ewma_ms)
#else
static UB dnode_incarn[DNODE_MAX];
#define SW_INC(n)         dnode_incarn[(n)]
#define SW_INC_SET(n, v)  (dnode_incarn[(n)] = (v))
#define SW_SUS(n)         suspect_count[(n)]
#define SW_SUS_SET(n, v)  (suspect_count[(n)] = (v))
#define SW_SUS_INC(n)     (suspect_count[(n)]++)
#define SWT_INC(n)        dnode_incarn[(n)]
#define SWT_SUS(n)        suspect_count[(n)]
#define SWT_RTTV(n)       rtt_valid[(n)]
#define SWT_RTT(n)        rtt_ewma_ms[(n)]
#endif

/* deterministic peer address (10.1.0.(n+1)); defined below, used by the
 * transitive-discovery path in gossip_apply() to resolve a gossip-introduced
 * peer's IP from its node id alone (no address rides the gossip frame). */
static UW swim_node_ip(pk_node_t n);
#if !defined(_TK_HOSTED_LIBC_)
static UB suspect_count[DNODE_MAX];    /* fwd: cleared on gossip discovery */
#endif

/* state severity ladder for same-incarnation tie-break (ALIVE<SUSPECT<DEAD) */
static INT state_rank(UB st)
{
    if (st == DNODE_DEAD)    return 2;
    if (st == DNODE_SUSPECT) return 1;
    return 0;   /* ALIVE / UNKNOWN */
}

/* galaxy v1 (galaxy.md S1): ONE file-static emitter called at each
 * existing SWIM state-change print site, so a star appears / dims / goes
 * dark in the window exactly when SWIM's belief changes — with SWIM's
 * latency, no new gossip (the membership table itself is read directly
 * by /galaxy.json). old==0xFF where the prior state was not in hand. */
static void sw_note(pk_node_t nid, UB oldst, UB newst)
{
    galaxy_emit(EV_SWIM, nid, GALAXY_NODE_NONE, (UH)oldst, (UH)newst);
}

/* N-2b: capability rides every queued entry. For gossip ABOUT SELF, callers
 * pass region_is_super_capable(drpc_my_node) (self-authoritative). For
 * re-propagated PEER rumors, callers relay the byte VERBATIM as received —
 * a node never originates a peer's capability. cap_self() is the one-liner
 * the self-gossip sites use so the source of truth stays in region.c. */
static UB cap_self(void)
{
    return (drpc_my_node != 0xFF && region_is_super_capable(drpc_my_node))
           ? 1 : 0;
}

/* T-fix-a (thread-t-impl-plan.md §2.3): teacher_self() mirrors cap_self() for
 * capability bit 1. It is TRUE iff this node actually holds a successfully
 * lm_load'ed teacher GGUF — a VERIFIABLE runtime property, NOT a bare env
 * decree (a lying node could otherwise self-elect; opt-in PKERNEL_TEACHER=1 is
 * gated by GGUF presence inside the hook, never sufficient alone).
 *
 * swim.c lives in arch/common/ and links on EVERY target (incl. bare-metal x86
 * & aarch64, which have NO llm/student layer), so it cannot call the llm tier
 * directly without breaking those links. So the truth-source is a WEAK hook —
 * the SAME pattern as interocept.c's intero_fault_count_hook() and
 * student_stub.c's weak student symbols: a weak default of 0 here, overridden
 * by a STRONG definition in the hosted student layer (student_shell.c) that
 * does a one-time, cached gguf_open+lm_load probe of the configured teacher
 * GGUF. On bare-metal/Android (no override) the weak 0 means "not a teacher" —
 * the safe degrade. HONEST BOUND: an in-kernel teacher GGUF is not yet wired
 * onto the boot path (that is T-1/CT-2, deferred), so on a stock hosted node
 * with no teacher GGUF present the probe fails and teacher_self()==0; the bit
 * goes high only on a node explicitly given a loadable teacher GGUF. */
__attribute__((weak)) int teacher_gguf_loaded(void) { return 0; }

#if defined(_TK_HOSTED_LIBC_)
/* wave-selfelect cert hook (hosted-only, off by default): the in-proc
 * self-elect regression cert sets this to simulate "this node holds a teacher
 * GGUF" without an actual model file — the SAME save/restore scaffold the other
 * swim self-tests use. On bare-metal this whole block is absent (the crown sees
 * the prior teacher_self() verbatim). */
static int selfelect_force_teacher = 0;
#endif

static UB teacher_self(void)
{
#if defined(_TK_HOSTED_LIBC_)
    if (selfelect_force_teacher) return (drpc_my_node != 0xFF) ? 1 : 0;
#endif
    return (drpc_my_node != 0xFF && teacher_gguf_loaded()) ? 1 : 0;
}

/* Self-authoritative capability BYTE for gossip ABOUT SELF: pack both axes
 * (bit0=supernode, bit1=teacher) from this node's own verifiable state.
 * Old nodes that never set bit 1 still emit a byte a new node reads as
 * teacher=0 → safe degrade. */
static UB cap_self_byte(void)
{
#if defined(_TK_HOSTED_LIBC_) && !defined(N2_NC_NOBIT2)
    /* N2 (ii)(a): bit 2 = this node reads v2 (every hosted build since N2b).
     * Old nodes read bits 0 and 1 only, so the extra bit changes nothing there. */
    return (UB)((cap_self() << 0) | (teacher_self() << 1) | (1u << 2));
#else
    return (UB)((cap_self() << 0) | (teacher_self() << 1));
#endif
}

/* wave-selfelect: SELF-RECOGNITION of own capability.
 *
 * region_teacher()/region_supernode() consult the teacher_capable[]/
 * super_capable[] TABLES, but those entries are set ONLY in the gossip-APPLY
 * path for RECEIVED gossip about a PEER. The self-beacon origination sites
 * (join/periodic/self-refute) only ENQUEUE the self capability byte for
 * broadcast (gossip_add appends to gq[]) — they never apply the bits to this
 * node's OWN local table. CONSEQUENCE: a teacher-capable node T broadcasts its
 * teacher bit so PEERS set teacher_capable[T] and elect T, but T ITSELF never
 * sets teacher_capable[T], so T's own region_teacher() never returns T → the
 * cradle teacher-gate (region_teacher()==self) never fires on the real teacher.
 *
 * Fix: at every self-origination site, ALSO apply the node's own verifiable
 * capability bits to its LOCAL region table, so the node self-recognizes the
 * role peers already compute for it. region_teacher()/region_supernode() still
 * pick the LOWEST-id capable MEMBER, so single-teacher/single-supernode per
 * region is preserved (a node emits only if it is the lowest-id capable one).
 *
 * CROWN SAFETY: swim.c links into BARE-METAL (boot/aarch64, boot/x86), whose
 * .text crown must stay byte-identical. On bare-metal cap_self()==0 and
 * teacher_self()==0 (no GGUF, no env) so the apply is a runtime NO-OP — but it
 * would still emit instructions and move .text. Therefore the body exists ONLY
 * under _TK_HOSTED_LIBC_; on bare-metal the macro expands to nothing → ZERO new
 * instructions, crown unchanged. SELFELECT_SKIP nulls the body (regression-cert
 * falsifier: with the self-apply gone, region_teacher()!=self). */
#if defined(_TK_HOSTED_LIBC_) && !defined(SELFELECT_SKIP)
#define SELF_APPLY_OWN_CAPABILITY()                                          \
    do {                                                                    \
        if (drpc_my_node != 0xFF) {                                         \
            region_set_super_capable(drpc_my_node,                          \
                                     cap_self() ? TRUE : FALSE);            \
            region_set_teacher_capable(drpc_my_node,                        \
                                       teacher_self() ? TRUE : FALSE);      \
        }                                                                   \
    } while (0)
#else
#define SELF_APPLY_OWN_CAPABILITY() do { } while (0)
#endif

/* Capability BYTE for RELAYING a PEER's already-converged capability (never
 * originates it): pack both axes from the local region tables, which were set
 * verbatim from that peer's own gossip under the LWW gate. Keeps bit 0
 * byte-identical to the prior region_is_super_capable(target)?1:0 relay while
 * carrying bit 1 forward so the teacher axis spreads epidemically too. */
#if defined(_TK_HOSTED_LIBC_)
/* N2 (ii)(a): capability bit 2 = "reads v2" (research/n2-wire16.md 2.2), per
 * narrow peer as that peer last said about itself. Wide peers read v2 by
 * construction (they exist only in v2). Hosted-only: bare neither sends nor
 * reads bit 2, so its .text does not move. */
/* N3 step 1: per slot (SWIM_PEER.v2cap); an id with no slot reads 0 */
static void v2cap_set(pk_node_t n, UB yes) { SWIM_PEER *p = sw_get(n); if (p) p->v2cap = yes ? 1 : 0; }
static UB   v2cap_get(pk_node_t n)         { SWIM_PEER *p = sw_get(n); return p ? p->v2cap : 0; }
/* adopt bit 2 from a peer's own rumor (the same LWW gate as bits 0 and 1) */
#if defined(N2_NC_V2IGNORE)
#define V2CAP_ADOPT(nid, capbyte) do { (void)(capbyte); } while (0)
#else
#define V2CAP_ADOPT(nid, capbyte) v2cap_set((nid), (UB)(((capbyte) >> 2) & 1))
#endif
#else
#define V2CAP_ADOPT(nid, capbyte) do { } while (0)
#endif

static UB cap_byte(pk_node_t node)
{
#if defined(_TK_HOSTED_LIBC_)
    return (UB)((region_is_super_capable(node)   ? 1 : 0) << 0
              | (region_is_teacher_capable(node) ? 1 : 0) << 1
              | (v2cap_get(node) ? 1 : 0) << 2);   /* N2 (ii)(a): relay bit 2 too */
#else
    return (UB)((region_is_super_capable(node)   ? 1 : 0) << 0
              | (region_is_teacher_capable(node) ? 1 : 0) << 1);
#endif
}

static void gossip_add(pk_node_t node_id, UB state, UB incarnation, UB capability)
{
    PK_NOSW_ENTER();      /* pk_nosw.h: swim_task and swim_rx both add */
    /* Update existing entry if present */
    for (INT i = 0; i < gq_cnt; i++) {
        if (gq[i].node_id == node_id) {
            gq[i].state       = state;
            gq[i].incarnation = incarnation;
            gq[i].capability  = capability;
            gq[i].remaining   = GOSSIP_TTL;
            PK_NOSW_LEAVE();
            return;
        }
    }
    if (gq_cnt < GOSSIP_Q_MAX) {
        gq[gq_cnt++] = (GOSSIP_QUEUED){ node_id, state, incarnation, GOSSIP_TTL, capability };
        PK_NOSW_LEAVE();
        return;
    }
    /* Queue full: evict oldest (lowest remaining) */
    INT min_i = 0;
    for (INT i = 1; i < gq_cnt; i++)
        if (gq[i].remaining < gq[min_i].remaining) min_i = i;
    gq[min_i] = (GOSSIP_QUEUED){ node_id, state, incarnation, GOSSIP_TTL, capability };
    PK_NOSW_LEAVE();
}

static void gossip_fill(SWIM_PKT *pkt)
{
    PK_NOSW_ENTER();      /* reads, decrements and compacts gq[] */
    pkt->gossip_cnt = 0;
    for (INT i = 0; i < gq_cnt && pkt->gossip_cnt < SWIM_GOSSIP_MAX; i++) {
        if (gq[i].remaining == 0) continue;
        pkt->gossip[pkt->gossip_cnt].node_id     = pk_node_v1(gq[i].node_id);
        pkt->gossip[pkt->gossip_cnt].state       = gq[i].state;
        pkt->gossip[pkt->gossip_cnt].incarnation = gq[i].incarnation;
        pkt->gossip[pkt->gossip_cnt].capability  = gq[i].capability;  /* N-2b */
        pkt->gossip_cnt++;
        gq[i].remaining--;
    }
    /* Compact retired entries */
    INT w = 0;
    for (INT i = 0; i < gq_cnt; i++)
        if (gq[i].remaining > 0) gq[w++] = gq[i];
    gq_cnt = w;
    PK_NOSW_LEAVE();
}

/* (incarnation, state) lexicographic last-writer-wins: does the incoming
 * rumor (inc_in, st_in) supersede what we currently hold (inc_cur, st_cur)?
 *   - strictly higher incarnation always wins;
 *   - same incarnation: a more-dead state wins (ALIVE<SUSPECT<DEAD);
 *   - strictly lower incarnation never wins (the stale rumor is refuted). */
static INT gossip_supersedes(UB inc_in, UB st_in, UB inc_cur, UB st_cur)
{
#if defined(_TK_HOSTED_LIBC_) && !defined(N2C_NC_PLAIN_CMP)
    /* N2c stage 2: serial-number order (RFC 1982) on the 1-byte incarnation:
     * a is newer than b when (UB)(a - b) is 1..127, so the refute 0 of a
     * DEAD@255 wins instead of losing forever. A gap of exactly 128 is
     * neither (the limit, research/n2c-identity.md §10). */
    if (inc_in != inc_cur) { UB d = (UB)(inc_in - inc_cur); return d >= 1 && d <= 127; }
#else
    if (inc_in != inc_cur) return inc_in > inc_cur;
#endif
    return state_rank(st_in) > state_rank(st_cur);
}

#if defined(_TK_HOSTED_LIBC_)
/* ------------------------------------------------------------------ */
/* DE-STORM throttle for the SELF-SUSPICION 断末魔 scatter (HOSTED-ONLY) */
/*                                                                     */
/* Under self-hosted CI CPU load a node routinely falls behind on SWIM  */
/* PING/ACK and gets FALSELY rumored SUSPECT. The self-suspect branch    */
/* below reacts to EVERY such rumor with replica_scatter_all() — a FULL, */
/* UNPACED memory snapshot to all DNODE_MAX peers. The scatter work makes */
/* the node fall FURTHER behind → more false suspicion → more scatter: a  */
/* self-feeding STORM (CI artifacts show 7-8 DEATH-THROES per node,       */
/* tracking SELF-SUSPICION 1:1). The deaf node then never folds a peer's  */
/* weight chunks (one_mind) nor consolidates/re-asks (shared_mind).       */
/*                                                                     */
/* Fix: THROTTLE the scatter to at most once per SELF_SCATTER_MIN_MS. The */
/* REFUTE (incarnation bump + ALIVE re-assert, below) STILL fires EVERY    */
/* time, so liveness/refutation is untouched. A genuinely dying node STILL */
/* scatters its memories — just once per interval instead of 8x in a burst.*/
/*                                                                     */
/* CROWN SAFETY: this whole block + the call-site guard live under          */
/* _TK_HOSTED_LIBC_; on bare-metal the scatter stays the unchanged           */
/* `replica_scatter_all();` — ZERO new .text emitted, crown byte-identical.  */
/* Falsifier -DSWIM_NO_SCATTER_THROTTLE reverts to the unthrottled storm so  */
/* the cert can prove the throttle has teeth. */
/* ------------------------------------------------------------------ */
#ifndef SELF_SCATTER_MIN_MS
#define SELF_SCATTER_MIN_MS 2000
#endif
static UW  self_scatter_last_ms = 0;
static INT self_scatter_primed  = 0;   /* have we ever scattered yet? */
static UW  self_scatter_fires   = 0;   /* cert reads this: # of ACTUAL scatters */

static INT self_scatter_due(void)
{
#if defined(SWIM_NO_SCATTER_THROTTLE)
    /* FALSIFIER: the pre-fix behaviour — every self-SUSPECT rumor scatters.
     * The cert then observes N fires for an N-rumor burst → RED. */
    self_scatter_fires++;
    return 1;
#else
    SYSTIM now; tk_get_otm(&now);
    UW now_ms = now.lo;   /* monotonic ms (same clock swim uses for RTT) */
    if (self_scatter_primed &&
        (UW)(now_ms - self_scatter_last_ms) < (UW)SELF_SCATTER_MIN_MS)
        return 0;                         /* within the interval: suppress */
    self_scatter_primed  = 1;
    self_scatter_last_ms = now_ms;
    self_scatter_fires++;
    return 1;
#endif
}
#endif /* _TK_HOSTED_LIBC_ */

static void gossip_apply(const SWIM_PKT *pkt)
{
    for (UB i = 0; i < pkt->gossip_cnt && i < SWIM_GOSSIP_MAX; i++) {
        pk_node_t nid    = pkt->gossip[i].node_id;
        UB st     = pkt->gossip[i].state;
        UB inc_in = pkt->gossip[i].incarnation;
        /* N-2b / T-fix-a: SELF-AUTHORITATIVE capability bits — meaningful only
         * when this entry is node `nid`'s own rumor about itself; we relay them
         * verbatim and never originate a peer's capability here.
         *   cap_in     = bit 0 (supernode-capable)
         *   teacher_in = bit 1 (teacher-capable)  — extracted under the SAME
         *                LWW gate so a stale rumor cannot flip it (mirror N-2b). */
        UB cap_in     = (pkt->gossip[i].capability >> 0) & 1;
        UB teacher_in = (pkt->gossip[i].capability >> 1) & 1;

        /* 自分自身が SUSPECT/DEAD と噂されていたら断末魔散布 +
         * incarnation を進めて ALIVE を再表明し、古い噂を論駁する。 */
        if (nid == drpc_my_node) {
            if (st == DNODE_SUSPECT || st == DNODE_DEAD) {
                sw_puts("[swim] *** SELF-SUSPICION *** I'm rumored ");
                sw_puts(st == DNODE_SUSPECT ? "SUSPECT" : "DEAD");
                sw_puts(" — refuting with fresh incarnation\r\n");
#if defined(_TK_HOSTED_LIBC_)
                if (self_scatter_due())   /* de-storm throttle (hosted-only) */
                    replica_scatter_all();
#else
                replica_scatter_all();
#endif
                /* refute: bump strictly above the rumor's incarnation so the
                 * ALIVE re-assertion supersedes the stale DEAD/SUSPECT
                 * everywhere it has spread (UB ++ wraps sanely at 256). */
#if defined(_TK_HOSTED_LIBC_) && !defined(N2C_NC_PLAIN_CMP)
                /* N2c stage 2: "at or past mine" in serial order, so the
                 * refute is newer than both the rumour and my own word. */
                if (inc_in == my_incarnation || gossip_supersedes(inc_in, DNODE_ALIVE, my_incarnation, DNODE_ALIVE))
                    my_incarnation = (UB)(inc_in + 1);
                else
                    my_incarnation = (UB)(my_incarnation + 1);
                {   /* the refuted incarnation is kept too (§10 4): a restart
                     * then starts past it (node_ident.c) */
                    extern void pk_node_incarn_note(unsigned char wire);
#if defined(N2C_NC_NOTE_NEVER)
                    /* falsifier (audit-201): the refute is never kept -> I8 red */
                    if (0)
#elif !defined(N2C_NC_NOTE_IN_TEST)
                    if (sw_selftest_depth == 0)
#endif
                        pk_node_incarn_note(my_incarnation);
                }
#else
                if (inc_in >= my_incarnation) my_incarnation = (UB)(inc_in + 1);
                else                          my_incarnation = (UB)(my_incarnation + 1);
#endif
                /* re-assert MY OWN capability bits, both axes (self-authoritative
                 * source: supernode + teacher). */
                gossip_add(drpc_my_node, DNODE_ALIVE, my_incarnation, cap_self_byte());
                SELF_APPLY_OWN_CAPABILITY();
            }
            continue;
        }

        if (nid >= DNODE_MAX) continue;
#if defined(_TK_HOSTED_LIBC_)
        /* N3 step 1: a rumour needs the peer's slot; none free = dropped */
        /* may push out a live holder: a live word about a peer not held DEAD, or a
         * word NEWER than the peer's grave (it came back: C9) */
        if (!sw_take_ex(nid, (st != DNODE_DEAD && (DROW(nid).state != DNODE_DEAD
                                                   || sw_grave_newer(nid, inc_in))) ? TRUE : FALSE))
            continue;
#endif

        /* NET-DISCOVERY-STAR (wave-discovery-mesh): TRANSITIVE membership.
         * A peer we have never heard of (UNKNOWN) cannot be introduced by the
         * (incarnation,state) LWW alone: state_rank(ALIVE)==state_rank(UNKNOWN)
         * and the rumour's incarnation usually ties ours (both 0 at bring-up),
         * so gossip_supersedes() returns false and the rumour is dropped. That
         * is the defect — standard SWIM disseminates MEMBERSHIP epidemically
         * (B tells A that C exists), but here a node could only ever learn a
         * peer from a DIRECT packet, capping the live mesh hub-centric.
         *
         * Fix: treat the FIRST non-DEAD sighting of an UNKNOWN peer as a
         * discovery — adopt it. The peer's address is deterministic
         * (node_base_ip / swim_node_ip == 10.1.0.(nid+1), pre-seeded into ARP
         * by drpc_init), so node_id alone resolves it; no address needs to ride
         * the gossip frame (no wire change). We seed dnode_table[nid].ip so any
         * later unicast (and `nodes`) has the right address even before our own
         * direct probe RTT lands. We do NOT fabricate an RTT: region_recompute
         * still requires a real swim_rtt_ms(), which the directed PING/ACK probe
         * measures directly within a couple of rounds. A DEAD rumour about
         * an UNKNOWN peer is ignored (we never saw it alive — gossiping it dead
         * would only seed a grave). */
        if (DROW(nid).state == DNODE_UNKNOWN &&
            (st == DNODE_ALIVE || st == DNODE_SUSPECT)) {
            DROWW(nid).node_id = nid;
            DROWW(nid).ip      = swim_node_ip(nid);
            DROWW(nid).state   = st;
            DROWW(nid).missed  = 0;
            SW_INC_SET(nid, inc_in);
            SW_SUS_SET(nid, 0);
            /* N-2b / T-fix-a: adopt the peer's self-declared capability bits
             * VERBATIM (bit0=supernode, bit1=teacher) and re-propagate them
             * (epidemic relay; we do not originate them). */
            region_set_super_capable(nid, cap_in ? TRUE : FALSE);
            region_set_teacher_capable(nid, teacher_in ? TRUE : FALSE);
            V2CAP_ADOPT(nid, pkt->gossip[i].capability);   /* N2 (ii)(a): bit 2 */
            sw_note(nid, DNODE_UNKNOWN, st);
            sw_puts("[swim] gossip: node "); sw_putdec(nid);
            sw_puts(st == DNODE_ALIVE ? " discovered (via gossip)\r\n"
                                      : " SUSPECT (via gossip)\r\n");
            gossip_add(nid, st, inc_in, cap_byte(nid));   /* re-propagate both axes: keep the epidemic alive */
            degrade_update();
            dmn_trigger();
            continue;
        }

        /* anti-stale: accept only if (incarnation,state) supersedes our view */
        if (!gossip_supersedes(inc_in, st, SW_INC(nid), DROW(nid).state)) {
#if defined(_TK_HOSTED_LIBC_) && !defined(N2_NC_V2DIRECT_NO)
            /* N2 (ii)(a), audit-147: an old relay drops bit 2, and the peer's
             * own word at the same incarnation loses the gate above, so take
             * bit 2 (only) from the peer itself (src == nid) at the incarnation
             * we hold. Third parties still cannot set it. */
            if (pkt->src_node == (pk_node8_t)nid && pkt->src_node != PK_NODE_NONE &&
                inc_in == SW_INC(nid))
                V2CAP_ADOPT(nid, pkt->gossip[i].capability);
#endif
            continue;   /* stale rumor refuted — capability does NOT regress */
        }

#if defined(_TK_HOSTED_LIBC_)
        UB inc_was = SW_INC(nid);   /* N2c stage 2: for the "back at" line */
#endif
        SW_INC_SET(nid, inc_in);
        /* N-2b / T-fix-a: a SUPERSEDING rumor (this peer's own fresher word
         * about itself) updates BOTH capability axes in LOCK-STEP with
         * membership, VERBATIM. A stale lower-incarnation rumor was rejected
         * above, so both bits converge under the exact same anti-stale gate as
         * state. Do this BEFORE the state-same short-circuit so a capability
         * flip carried by a fresh incarnation lands even when state is same. */
        region_set_super_capable(nid, cap_in ? TRUE : FALSE);
        region_set_teacher_capable(nid, teacher_in ? TRUE : FALSE);
        V2CAP_ADOPT(nid, pkt->gossip[i].capability);       /* N2 (ii)(a): bit 2 */

        if (DROW(nid).state == st) {
            /* incarnation bumped, state same: still relay the fresh entry so
             * the (possibly changed) capability keeps spreading epidemically. */
            gossip_add(nid, st, inc_in, cap_byte(nid));
            continue;
        }

        { UB oldst = DROW(nid).state; DROWW(nid).state = st;
          sw_note(nid, oldst, st);
#if defined(_TK_HOSTED_LIBC_)
          /* N2c stage 2: the only way back from DEAD is the peer's own newer
           * incarnation (rx, probe answers and heartbeats no longer revive). */
          if (oldst == DNODE_DEAD && st == DNODE_ALIVE) {
              sw_puts("[swim] node "); sw_putdec(nid);
              sw_puts(" back at incarnation "); sw_putdec(inc_in);
              sw_puts(" (dead at "); sw_putdec(inc_was); sw_puts(")\r\n");
              DROWW(nid).missed = 0;
              SW_SUS_SET(nid, 0);
              gossip_add(nid, st, inc_in, cap_byte(nid));
              replica_push_to(nid);
              degrade_update();
              dmn_trigger();
              continue;
          }
#endif
        }
        sw_puts("[swim] gossip: node "); sw_putdec(nid);
        if      (st == DNODE_ALIVE)   sw_puts(" -> ALIVE\r\n");
        else if (st == DNODE_SUSPECT) sw_puts(" -> SUSPECT\r\n");
        else if (st == DNODE_DEAD)    sw_puts(" -> DEAD\r\n");
        gossip_add(nid, st, inc_in, cap_byte(nid));   /* re-propagate both axes at the same incarnation */
        degrade_update();
        dmn_trigger();   /* ノード状態変化 = 外部刺激 */
    }
}

/* ------------------------------------------------------------------ */
/* Probe state                                                         */
/* ------------------------------------------------------------------ */

static ID probe_sem          = -1;     /* signalled by swim_rx on ACK    */
static UH probe_seq          = 1;
static pk_node_t probe_waiting_node = 0xFF;   /* node currently being probed    */
/* suspect_count[] defined above (consecutive no-response rounds) — moved up so
 * the transitive-discovery path in gossip_apply() can clear it on adoption. */

/* ------------------------------------------------------------------ */
/* RTT estimation (R0, regions design — docs/architecture/20-architecture/regions.md)  */
/*                                                                     */
/* 直接プローブ (SWIM_PING→SWIM_ACK) の往復時間を測り、ノードごとに      */
/* EWMA (alpha=1/4) で平滑化する。間接プローブはヘルパー経由で対象との    */
/* 直接 RTT にならないので測定対象外。region 形成と locality-aware MoE の  */
/* ゲーティングがこの値を消費する。                                      */
/* ------------------------------------------------------------------ */
#if !defined(_TK_HOSTED_LIBC_)   /* hosted: SWIM_PEER.rtt_ewma_ms / rtt_valid (N3) */
static UW rtt_ewma_ms[DNODE_MAX];      /* 平滑化済み RTT (ms)            */
static UB rtt_valid  [DNODE_MAX];      /* 1 = 一度でも実測した           */
#endif

/* --- RTT シミュレーション (テスト専用; 本番では無効) ---------------- *
 * localhost では全ノードの実測 RTT がほぼ同じ (~数 ms) になり region が
 * 分割されない。region 形成ロジックを degenerate でなく検証するため、
 * ノード ID を zone_size でグループ化し (zone = id / zone_size)、異なる
 * zone のピアへ合成ペナルティを上乗せして観測 RTT を水増しする。
 * arch 非依存に保つため env は読まない: linux usermain が
 * swim_set_sim_zone() で注入する。zone_size==0 で無効。 */
static UB sim_zone_size    = 0;
static UW sim_zone_penalty = 0;

void swim_set_sim_zone(UB zone_size, UW penalty_ms)
{
    sim_zone_size    = zone_size;
    sim_zone_penalty = penalty_ms;
}

static void rtt_observe(pk_node_t node, UW sample_ms)
{
#if defined(_TK_HOSTED_LIBC_)
    SWIM_PEER *p = sw_get(node);   /* N3: a peer with no slot is not measured */
    if (!p) return;
    if (!p->rtt_valid) {
        p->rtt_ewma_ms = sample_ms;
        p->rtt_valid   = 1;
    } else {
        p->rtt_ewma_ms = (p->rtt_ewma_ms * 3 + sample_ms + 2) / 4;
    }
#else
    if (node >= DNODE_MAX) return;
    if (!rtt_valid[node]) {
        rtt_ewma_ms[node] = sample_ms;
        rtt_valid[node]   = 1;
    } else {
        /* ewma = 3/4 old + 1/4 sample (+2 は四捨五入) */
        rtt_ewma_ms[node] = (rtt_ewma_ms[node] * 3 + sample_ms + 2) / 4;
    }
#endif
}

/* 公開: ノードへの平滑化 RTT(ms)。未実測は 0xFFFFFFFF を返す。
 * sim_zone が有効なら異 zone のピアへ合成ペナルティを上乗せする。 */
UW swim_rtt_ms(pk_node_t node)
{
#if defined(_TK_HOSTED_LIBC_)
    UW r;
    if (node >= DNODE_MAX) {               /* N2b: a wide peer, v2 PING/ACK */
        r = swim_wide_rtt(node);
        if (r == 0xFFFFFFFFUL) return r;
    } else {
        SWIM_PEER *p = sw_get(node);   /* N3: no slot = not measured */
        if (!p || !p->rtt_valid) return 0xFFFFFFFFUL;
        r = p->rtt_ewma_ms;
    }
#else
    if (node >= DNODE_MAX || !rtt_valid[node]) return 0xFFFFFFFFUL;
    UW r = rtt_ewma_ms[node];
#endif
    if (sim_zone_size > 0 && drpc_my_node != 0xFF &&
        node / sim_zone_size != drpc_my_node / sim_zone_size) {
        r += sim_zone_penalty;
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static UW swim_node_ip(pk_node_t n)
{
    /* 10.1.0.(n+1) — same scheme as drpc */
    return pk_node_ip(n);
}

static void swim_send(UW dst_ip, SWIM_PKT *pkt)
{
    gossip_fill(pkt);
    udp_send(dst_ip, SWIM_PORT, SWIM_PORT, (const UB *)pkt, (UH)sizeof(*pkt));
}

/* ------------------------------------------------------------------ */
/* UDP receive callback (registered on SWIM_PORT)                     */
/* ------------------------------------------------------------------ */

#if defined(_TK_HOSTED_LIBC_)
static void swim_wide_init(void);
static INT  swim_wide_rx(UW src_ip, const UB *data, UH len);
static void swim_wide_tick(void);
static void swim_wide_print(void);
#endif

void swim_rx(UW src_ip, UH src_port, const UB *data, UH len)
{
    (void)src_port;
#if defined(_TK_HOSTED_LIBC_)
    if (swim_wide_rx(src_ip, data, len)) return;
#endif
    if (len < (UH)sizeof(SWIM_PKT)) return;

    const SWIM_PKT *pkt = (const SWIM_PKT *)data;
    if (pkt->magic != SWIM_MAGIC || pkt->version != SWIM_VERSION) return;

    /* Apply piggybacked gossip */
    gossip_apply(pkt);

    /* Mark sender alive */
    pk_node_t snid = pkt->src_node;
#if defined(_TK_HOSTED_LIBC_) && !defined(N2C_NC_RX_REVIVE)
    /* N2c stage 2 (§10 1, 2): a packet from a peer we hold DEAD@k does not
     * bring it back (its own newer incarnation does, in gossip_apply above).
     * Tell it instead: queue DEAD@k so it hears it and refutes with k+1.
     * gossip_add renews the TTL on every such packet, so a peer that never
     * refutes (an old build) keeps the rumour going (the limit, §10). */
    if (snid < DNODE_MAX && snid != drpc_my_node && DROW(snid).state == DNODE_DEAD) {
#if !defined(N2C_NC_NO_TELL)
        gossip_add(snid, DNODE_DEAD, SW_INC(snid), cap_byte(snid));
#endif
        snid = DNODE_MAX;   /* skip "mark sender alive"; still answer below */
    }
#endif
#if defined(_TK_HOSTED_LIBC_)
    /* N3 step 1: SWIM keeps a sender it has a slot for; else it only answers */
    if (snid < DNODE_MAX && snid != drpc_my_node && !sw_take(snid))
        snid = DNODE_MAX;
#endif
    if (snid < DNODE_MAX && snid != drpc_my_node) {
        UB old = DROW(snid).state;
        DROWW(snid).ip     = src_ip;
        DROWW(snid).state  = DNODE_ALIVE;
        if (old != DNODE_ALIVE) sw_note(snid, old, DNODE_ALIVE);
        DROWW(snid).missed = 0;
        SW_SUS_SET(snid, 0);
        if (old != DNODE_ALIVE) {
            sw_puts("[swim] node "); sw_putdec(snid);
            sw_puts(old == DNODE_UNKNOWN ? " discovered" : " recovered");
            sw_puts("  (via rx)\r\n");
            /* N-2b/T-fix-a: relay the peer's last-known capability VERBATIM on
             * BOTH axes (bit0=supernode, bit1=teacher; we do not originate it,
             * only the peer's own rumor changes it). This rx-rediscovery relay
             * MUST carry bit 1 too — dropping it would re-propagate a genuine
             * teacher as teacher=0 for GOSSIP_TTL rounds at the same incarnation,
             * and a third node that FIRST learns the peer through this relay
             * would latch teacher=FALSE until an incarnation bump (the LWW gate
             * drops the teacher's own same-incarnation correcting beacon). */
            gossip_add(snid, DNODE_ALIVE, SW_INC(snid), cap_byte(snid));
            replica_push_to(snid);
            degrade_update();
        }
    }

    switch (pkt->type) {

    case SWIM_PING: {
        /* Reply ACK directly to sender */
        SWIM_PKT ack = { 0 };
        ack.magic        = SWIM_MAGIC;
        ack.version      = SWIM_VERSION;
        ack.type         = SWIM_ACK;
        ack.seq          = pkt->seq;
        ack.src_node     = pk_node_v1(drpc_my_node);
        ack.probe_target = pk_node_v1(drpc_my_node);
        swim_send(src_ip, &ack);
        break;
    }

    case SWIM_ACK: {
        /* Wake probing task if seq + target match */
        if (probe_sem >= 0
            && pkt->seq          == probe_seq
            && pkt->probe_target == probe_waiting_node) {
            tk_sig_sem(probe_sem, 1);
        }
        break;
    }

    case SWIM_PING_REQ: {
        /*
         * Helper role: probe probe_target on behalf of src_node.
         * Forward SWIM_PING with original seq so the target's ACK
         * (sent back to us) can be forwarded to the original prober.
         * Simplification: we forward the PING; if the target is alive
         * it will send ACK to us, and we forward it to the requester.
         */
        pk_node_t target = pkt->probe_target;
        if (target >= DNODE_MAX || target == drpc_my_node) break;

        /* Ping the target */
        SWIM_PKT fwd = { 0 };
        fwd.magic        = SWIM_MAGIC;
        fwd.version      = SWIM_VERSION;
        fwd.type         = SWIM_PING;
        fwd.seq          = pkt->seq;     /* preserve seq for ACK matching */
        fwd.src_node     = pk_node_v1(drpc_my_node);
        fwd.probe_target = pk_node_v1(target);
        swim_send(swim_node_ip(target), &fwd);

        /*
         * Note: target's SWIM_ACK comes back to us (we sent the PING).
         * swim_rx will receive the ACK → if seq matches probe_seq and
         * probe_waiting_node matches, it wakes the original prober's sem.
         * This works because the original prober keeps probe_waiting_node
         * set while waiting on probe_sem.
         */
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Round-robin probe target selector                                   */
/* ------------------------------------------------------------------ */

#if defined(_TK_HOSTED_LIBC_)
/* N3 step 1: round-robin over the SLOTS (O(R)); a peer with no slot is not
 * probed. The fallback (nobody known yet) is the lowest non-self id, as before. */
static INT probe_cursor = 0;

static pk_node_t pick_probe_target(void)
{
#if defined(N3_NC_NO_PROBE_SLOTS)
    /* negative control of [n3-swim-cap] C6: the slots are never probed, so
     * SWIM never finds a dead slot holder (only the fallback below runs) */
    for (INT tries = 0; tries < 0; tries++) {
#else
    for (INT tries = 0; tries < SW_STORE; tries++) {
#endif
        probe_cursor = (probe_cursor + 1) % SW_STORE;
        if (!sw_map.used[probe_cursor]) continue;
        pk_node_t id = (pk_node_t)sw_map.id[probe_cursor];
        if (id == drpc_my_node) continue;
        if (DROW(id).state != DNODE_UNKNOWN) return id;
    }
    for (INT i = 0; i < DNODE_MAX; i++) {
        if ((pk_node_t)i != drpc_my_node) return (pk_node_t)i;
    }
    return drpc_my_node;
}

static void pick_helpers(pk_node_t exclude, pk_node_t helpers[SWIM_K_HELPERS], INT *cnt)
{
    *cnt = 0;
    for (INT i = 0; i < SW_STORE && *cnt < SWIM_K_HELPERS; i++) {
        if (!sw_map.used[i]) continue;
        pk_node_t id = (pk_node_t)sw_map.id[i];
        if (id == drpc_my_node || id == exclude) continue;
        if (DROW(id).state == DNODE_ALIVE) helpers[(*cnt)++] = id;
    }
}
#else
static UB probe_cursor = 0;

static pk_node_t pick_probe_target(void)
{
    /* Try round-robin among known (non-unknown) nodes first */
    for (UB tries = 0; tries < DNODE_MAX; tries++) {
        probe_cursor = (UB)((probe_cursor + 1) % DNODE_MAX);
        if (probe_cursor == drpc_my_node) continue;
        if (DROW(probe_cursor).state != DNODE_UNKNOWN) return probe_cursor;
    }
    /* Fallback: any non-self node (trigger initial discovery) */
    for (UB i = 0; i < DNODE_MAX; i++) {
        if (i != drpc_my_node) return i;
    }
    return drpc_my_node;
}

static void pick_helpers(pk_node_t exclude, pk_node_t helpers[SWIM_K_HELPERS], INT *cnt)
{
    *cnt = 0;
    for (UB i = 0; i < DNODE_MAX && *cnt < SWIM_K_HELPERS; i++) {
        if (i == drpc_my_node || i == exclude) continue;
        if (DROW(i).state == DNODE_ALIVE) helpers[(*cnt)++] = i;
    }
}
#endif

#if defined(_TK_HOSTED_LIBC_)
/* A probe of `target` was answered (directly or through a helper). One
 * function on hosted so the N2c `nodes life` test (I7 c) drives the real
 * code. Bare metal keeps the two written-out copies in swim_task: moving
 * them into a function (even always_inline) changed the aarch64 .text. */
static void swim_probe_ok(pk_node_t target, const char *how)
{
#if defined(_TK_HOSTED_LIBC_) && !defined(N2C_NC_RX_REVIVE)
    /* N2c stage 2: an answer from a DEAD peer does not revive it; the answer
     * itself went through swim_rx, which told it (DEAD@k) */
    if (DROW(target).state == DNODE_DEAD) return;
#endif
    if (DROW(target).state != DNODE_ALIVE) {
        UB oldst = DROW(target).state;
        DROWW(target).state = DNODE_ALIVE;
        sw_note(target, oldst, DNODE_ALIVE);
        sw_puts("[swim] node "); sw_putdec(target);
        sw_puts(how);
        gossip_add(target, DNODE_ALIVE, SW_INC(target),
                   cap_byte(target));  /* N-2b/T-fix-a relay (both axes) */
        replica_push_to(target);
    }
    SW_SUS_SET(target, 0);
}
#endif /* _TK_HOSTED_LIBC_ */

/* ------------------------------------------------------------------ */
/* SWIM task — one probing round per SWIM_PROBE_INTERVAL_MS           */
/* ------------------------------------------------------------------ */

void swim_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 1 };
    probe_sem = tk_cre_sem(&cs);

    for (;;) {
        tk_dly_tsk(SWIM_PROBE_INTERVAL_MS);
#if defined(_TK_HOSTED_LIBC_)
        swim_wide_tick();
#endif
        if (drpc_my_node == 0xFF) continue;

        /* NET-DISCOVERY-STAR (wave-discovery-mesh): broadcast membership beacon.
         *
         * The defect: a peer left UNKNOWN until a DIRECT packet arrived, and the
         * only direct traffic a non-hub node reliably saw was its own directed
         * probe — whose all-UNKNOWN fallback always targeted the minimum-id node
         * (the hub). So two NON-hub peers never exchanged a direct packet and the
         * live mesh collapsed into a star centred on the lowest id; region_recompute
         * kept the two at size=1, capping the cluster hub-centric and blocking
         * federation.
         *
         * Fix (peer-symmetric, no central node, no wire change): once per round
         * each node emits ONE SWIM packet to the LIMITED BROADCAST address. Over
         * the relay (a broadcast fan-out bus) and over QEMU socket networking this
         * reaches EVERY node; swim_rx() already (a) marks the SENDER ALIVE on any
         * SWIM packet and (b) applies the piggybacked gossip. So a single beacon
         * makes A learn B directly AND carries B's knowledge of C to A
         * (transitive membership / infection-style dissemination). It is typed
         * SWIM_ACK with seq=0 so no receiver treats it as a probe to answer (no
         * ACK storm) and it never matches an outstanding probe's (seq,target).
         * Cost: one extra datagram per node per second — bounded and tiny. The
         * directed PING/ACK probing below is UNTOUCHED, so RTT measurement and
         * SUSPECT/DEAD liveness detection keep their exact prior cadence. */
        /* N-2b: seed/refresh SELF ALIVE gossip carrying MY OWN capability bit
         * (self-authoritative source = region_is_super_capable(self), set from
         * PKERNEL_SUPERNODE=1 by region_super_init). gossip_add merges onto the
         * existing self-entry (resets its TTL) so the queue does not grow, and
         * the beacon's gossip_fill below piggybacks it — newly-joined peers
         * thus learn my capability epidemically, in lock-step with membership,
         * with no extra packet. region_super_init() is idempotent. */
        region_super_init();
        /* T-fix-a: seed/refresh SELF ALIVE gossip carrying BOTH capability
         * bits (bit0=supernode via region_is_super_capable, bit1=teacher via
         * teacher_self()'s GGUF probe). Self-authoritative on both axes. */
        gossip_add(drpc_my_node, DNODE_ALIVE, my_incarnation, cap_self_byte());
        SELF_APPLY_OWN_CAPABILITY();

        {
            SWIM_PKT beacon = { 0 };
            beacon.magic        = SWIM_MAGIC;
            beacon.version      = SWIM_VERSION;
            beacon.type         = SWIM_ACK;   /* seq=0 => not a probe reply to anyone */
            beacon.seq          = 0;
            beacon.src_node     = pk_node_v1(drpc_my_node);
            beacon.probe_target = pk_node_v1(drpc_my_node);
            swim_send(IP4(255, 255, 255, 255), &beacon);
        }

        pk_node_t target = pick_probe_target();
        if (target == drpc_my_node) continue;

        /* ---- Step 1: direct SWIM_PING ---- */
        probe_waiting_node = target;
        probe_seq++;
        if (probe_seq == 0) probe_seq = 1;

        SWIM_PKT ping = { 0 };
        ping.magic        = SWIM_MAGIC;
        ping.version      = SWIM_VERSION;
        ping.type         = SWIM_PING;
        ping.seq          = probe_seq;
        ping.src_node     = pk_node_v1(drpc_my_node);
        ping.probe_target = pk_node_v1(target);
        swim_send(swim_node_ip(target), &ping);
        SYSTIM t_ping; tk_get_otm(&t_ping);   /* RTT 計測開始 */

        ER er = tk_wai_sem(probe_sem, 1, SWIM_PROBE_TMO_MS);
        if (er == E_OK) {
            /* Direct probe succeeded — clean round-trip, record RTT. */
            SYSTIM t_ack; tk_get_otm(&t_ack);
            rtt_observe(target, (UW)(t_ack.lo - t_ping.lo));
#if defined(_TK_HOSTED_LIBC_)
            swim_probe_ok(target, " -> ALIVE (direct probe)\r\n");
#else   /* bare metal: the same steps written out (crown: one function moved .text) */
            if (DROW(target).state != DNODE_ALIVE) {
                UB oldst = DROW(target).state;
                DROWW(target).state = DNODE_ALIVE;
                sw_note(target, oldst, DNODE_ALIVE);
                sw_puts("[swim] node "); sw_putdec(target);
                sw_puts(" -> ALIVE (direct probe)\r\n");
                gossip_add(target, DNODE_ALIVE, SW_INC(target),
                           cap_byte(target));  /* N-2b/T-fix-a relay (both axes) */
                replica_push_to(target);
            }
            SW_SUS_SET(target, 0);
#endif
            continue;
        }

        /* ---- Step 2: indirect probe via K helpers ---- */
        pk_node_t helpers[SWIM_K_HELPERS];
        INT hcnt = 0;
        pick_helpers(target, helpers, &hcnt);

        for (INT h = 0; h < hcnt; h++) {
            SWIM_PKT preq = { 0 };
            preq.magic        = SWIM_MAGIC;
            preq.version      = SWIM_VERSION;
            preq.type         = SWIM_PING_REQ;
            preq.seq          = probe_seq;
            preq.src_node     = pk_node_v1(drpc_my_node);
            preq.probe_target = pk_node_v1(target);
            swim_send(swim_node_ip(helpers[h]), &preq);
        }

        er = tk_wai_sem(probe_sem, 1, SWIM_IND_TMO_MS);
        if (er == E_OK) {
#if defined(_TK_HOSTED_LIBC_)
            swim_probe_ok(target, " -> ALIVE (indirect probe)\r\n");
#else   /* bare metal: the same steps written out (crown: one function moved .text) */
            if (DROW(target).state != DNODE_ALIVE) {
                UB oldst = DROW(target).state;
                DROWW(target).state = DNODE_ALIVE;
                sw_note(target, oldst, DNODE_ALIVE);
                sw_puts("[swim] node "); sw_putdec(target);
                sw_puts(" -> ALIVE (indirect probe)\r\n");
                gossip_add(target, DNODE_ALIVE, SW_INC(target),
                           cap_byte(target));  /* N-2b/T-fix-a relay (both axes) */
                replica_push_to(target);
            }
            SW_SUS_SET(target, 0);
#endif
            continue;
        }

        /* ---- Step 3: no response — escalate state ---- */
        SW_SUS_INC(target);
        UB st = DROW(target).state;

        /* UNKNOWN nodes are not "suspected dead" — we've simply never
         * confirmed them alive yet. Gossiping a never-seen node as SUSPECT
         * makes the peer self-suspect and trigger death throes during the
         * normal discovery race (esp. before ARP/relay routes warm up), so
         * leave UNKNOWN nodes untouched and let an actual rx mark them ALIVE.
         *
         * For ALIVE nodes, require SWIM_SUSPECT_ROUNDS consecutive misses
         * before SUSPECT: with only 2 nodes there are no indirect helpers,
         * so a single dropped UDP datagram must not be a death sentence. */
        if (st == DNODE_ALIVE && SW_SUS(target) >= SWIM_SUSPECT_LIMIT) {
            DROWW(target).state  = DNODE_SUSPECT;
            sw_note(target, DNODE_ALIVE, DNODE_SUSPECT);
            DROWW(target).missed = 0;
            SW_SUS_SET(target, 0);   /* count fresh toward DEAD */
            sw_puts("[swim] node "); sw_putdec(target);
            sw_puts(" -> SUSPECT (no response)\r\n");
            gossip_add(target, DNODE_SUSPECT, SW_INC(target),
                       cap_byte(target));  /* N-2b/T-fix-a relay (both axes) */
        } else if (st == DNODE_SUSPECT && SW_SUS(target) >= SWIM_DEAD_LIMIT) {
            DROWW(target).state  = DNODE_DEAD;
            sw_note(target, DNODE_SUSPECT, DNODE_DEAD);
            DROWW(target).missed = 0;
            SW_SUS_SET(target, 0);
            sw_puts("[swim] node "); sw_putdec(target);
            sw_puts(" -> DEAD\r\n");
            gossip_add(target, DNODE_DEAD, SW_INC(target),
                       cap_byte(target));  /* N-2b/T-fix-a relay (both axes) */
            heal_on_node_dead(target);
            degrade_update();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                      */
/* ------------------------------------------------------------------ */

void swim_init(void)
{
#if defined(_TK_HOSTED_LIBC_)
    nodemap_init(&sw_map, SWIM_PEERS);
#if defined(N3_NC_NO_CAP)
    sw_map.cap = DREGION_MAX;      /* negative control: no cap */
#endif
    for (INT i = 0; i < SW_STORE; i++) sw_peer[i] = (SWIM_PEER){ 0 };
    sw_refused = 0;
#else
    for (INT i = 0; i < DNODE_MAX; i++) { suspect_count[i] = 0; dnode_incarn[i] = 0; }
#endif
    my_incarnation = 0;
#ifdef _TK_HOSTED_LIBC_
    swim_rounds_from_env();
    swim_wide_init();
    {   /* N2c: the incarnation survives a restart ($PKERNEL_PFS_DIR/node.incarn,
         * written before the first beacon; node_ident.c). The wire keeps 1 byte. */
        extern unsigned pk_node_incarn_boot(void);
        unsigned v = pk_node_incarn_boot();
        if (v) {
            my_incarnation = (UB)(v & 0xFFu);
            sw_puts("[swim] incarnation "); sw_putdec((UW)v); sw_puts(" (node.incarn)\r\n");
        }
    }
#endif
    udp_bind(SWIM_PORT, swim_rx);
    sw_puts("[swim] SWIM ready  port=7375\r\n");
}

/* ------------------------------------------------------------------ */
/* Cluster display (shell `nodes`)                                     */
/* ------------------------------------------------------------------ */

static const char *state_str(UB st)
{
    switch (st) {
    case DNODE_ALIVE:   return "ALIVE  ";
    case DNODE_SUSPECT: return "SUSPECT";
    case DNODE_DEAD:    return "DEAD   ";
    default:            return "unknown";
    }
}

void swim_nodes_print(void)
{
    sw_puts("[cluster]  ID  State    IP              (SWIM + DRPC)\r\n");
#if defined(_TK_HOSTED_LIBC_)
    for (INT n = 0; n < DNODE_MAX; n++) {   /* N3: no 8-bit counter to DNODE_MAX */
#else
    for (UB n = 0; n < DNODE_MAX; n++) {
#endif
        if (n == drpc_my_node) {
            sw_puts("            "); sw_putdec(n);
            sw_puts("  SELF     "); sw_puts(ip_str(NET_MY_IP)); sw_puts("\r\n");
        } else if (DROW(n).state != DNODE_UNKNOWN) {
            sw_puts("            "); sw_putdec(n);
            sw_puts("  "); sw_puts(state_str(DROW(n).state));
            sw_puts("  "); sw_puts(ip_str(DROW(n).ip));
            {
                UW rtt = swim_rtt_ms(n);   /* sim_zone ペナルティ込み */
                if (rtt != 0xFFFFFFFFUL) {
                    sw_puts("  rtt="); sw_putdec(rtt); sw_puts("ms");
                } else {
                    sw_puts("  rtt=?");
                }
            }
            if (DROW(n).state == DNODE_SUSPECT) {
                sw_puts("  (suspect_cnt="); sw_putdec(SW_SUS(n)); sw_puts(")");
            }
            sw_puts("\r\n");
        }
    }
#if defined(_TK_HOSTED_LIBC_)
    swim_wide_print();
    /* N3 step 1: slots used / capacity, and how often an id found them full */
    sw_puts("[swim] slots "); sw_putdec(nodemap_count(&sw_map));
    sw_puts("/"); sw_putdec(SWIM_PEERS);
    sw_puts(" refused "); sw_putdec(sw_refused);
    sw_puts(" ids");                     /* who holds a slot ([n3-swim-cap] C6) */
    for (INT i = 0; i < SW_STORE; i++)
        if (sw_map.used[i]) { sw_puts(" "); sw_putdec(sw_map.id[i]); }
    sw_puts("\r\n");
#endif
}

#if defined(_TK_HOSTED_LIBC_)
/* ------------------------------------------------------------------ */
/* Wide ids in the membership (N2b, research/n2-wire16.md)             */
/*                                                                     */
/* A wide node beacons a v2 packet each round; a node that knows wide  */
/* ids beacons them too, so the news spreads. The v2 beacon is not a   */
/* probe: a wide peer is ALIVE while its own beacons arrive and ages   */
/* to SUSPECT and DEAD without them. A rumour can introduce a wide id  */
/* but never keeps one alive or kills one.                             */
/* ------------------------------------------------------------------ */
#define SWIM_WIDE_SUSPECT_MS 6000
#define SWIM_WIDE_DEAD_MS    15000

/* A wide id cannot index a table sized DNODE_MAX, so it has a slot here. Only
 * this membership list knows wide peers so far: no other subsystem sees one.
 * A wide node itself runs drpc as its wide id (usermain, pk_node_row.h). */
typedef struct {
    UW  ip;
    UW  heard_ms;   /* last packet from this node itself */
    UB  state;      /* DNODE_UNKNOWN / ALIVE / SUSPECT / DEAD */
    UB  rtt_ok;     /* rtt_ms holds a measurement                */
    UH  ping_seq;   /* seq of the v2 PING in flight, 0 = none    */
    UW  ping_ms;    /* when that PING went out                   */
    UW  rtt_ms;     /* smoothed round trip, as rtt_ewma_ms in v1  */
    UB  fp[8];      /* N2c stage 3: the key heard from it (fp_ok)  */
    UB  fp_ok;
    UB  fp_was[8];  /* the key before the last change              */
    UB  was_left;   /* news still to come after it (1st, 3rd carry fp_was; 2nd, 4th until 11-23) */
} SWIM_WIDE;

static NODEMAP   wide_map;          /* wide id -> slot, parallel to wide_st */
static SWIM_WIDE wide_st[DREGION_MAX];
static UW        nm_full;           /* wide ids refused: the table was full */
static UW        swim2_bad;         /* v2 packets dropped: too short        */
static pk_node_t swim_wide_me;      /* 0 = this node is not wide            */
static INT       swim_wide_cursor;
static INT       swim_wide_pcur;    /* whom the next v2 PING goes to         */
static UH        swim_wide_seq;     /* v2 PING numbers, never 0 (0 = beacon) */

/* N2 (ii)(a): may a one-node v2 frame go to `id`? */
BOOL swim_peer_reads_v2(pk_node_t id)
{
    if (id < DNODE_MAX) return v2cap_get(id) ? TRUE : FALSE;
#if defined(N2_NC_V2WIDE_NO)
    return FALSE;   /* falsifier: wide peers treated as unable to read v2 */
#endif
    /* a wide peer exists only in v2: known and not dead is enough */
    INT s = nodemap_find(&wide_map, id);
    return (s != NODEMAP_NOSLOT && wide_st[s].state != DNODE_DEAD
            && wide_st[s].state != DNODE_UNKNOWN) ? TRUE : FALSE;
}

/* Slot of a wide id, taken if new (a dead entry gives way when full), or -1. */
static INT wide_add(pk_node_t id)
{
    INT s = nodemap_find(&wide_map, id);
    if (s != NODEMAP_NOSLOT) return s;
    if (!PK_NODE_IS_WIDE(id)) return -1;
    s = nodemap_admit(&wide_map, id);
    for (INT i = 0; s == NODEMAP_NOSLOT && i < DREGION_MAX; i++) {
        if (wide_map.used[i] && wide_st[i].state == DNODE_DEAD) {
            nodemap_evict(&wide_map, i);
            s = nodemap_admit(&wide_map, id);
        }
    }
    if (s == NODEMAP_NOSLOT) { nm_full++; return -1; }
    wide_st[s].ip = 0;
    wide_st[s].heard_ms = 0;
    wide_st[s].state = DNODE_UNKNOWN;
    wide_st[s].rtt_ok = 0; wide_st[s].ping_seq = 0;
    wide_st[s].ping_ms = 0; wide_st[s].rtt_ms = 0;
    wide_st[s].fp_ok = 0; wide_st[s].was_left = 0;
    return s;
}

static UW swim_wide_now(void)
{
    SYSTIM t; tk_get_otm(&t);
    return t.lo;
}

static void swim_wide_init(void) { nodemap_init(&wide_map, 0); }

void swim_wide_self(pk_node_t id, UW ip)
{
    swim_wide_me = id;
    NET_MY_IP    = ip;
}

/* N2c stage 3 (research/n2c-identity.md §11): the same id is the same key.
 * A wide beacon carries a trailer after the 29-byte packet (an old node reads
 * only the 29 bytes): "N2F", a count n, then n fingerprints of 8 bytes (the
 * first 8 bytes of sha256 of the public key): [0] the sender's own (zeros when
 * the sender is narrow), [1 + i] the key this node keeps for gossip[i] (zeros
 * when it keeps none). The first key heard straight from a wide id is kept; a
 * beacon with another key is another node under that id: its round trip is
 * dropped, the new key is kept, one line. A node that hears its own wide id
 * with another key redraws its id when its own key is the larger (§2 3). */
#define SWIM2_FP_N (1 + SWIM_GOSSIP_MAX)
typedef struct {
    UB tag[3];             /* 'N' '2' 'F' */
    UB n;
    UB fp[SWIM2_FP_N][8];
} __attribute__((packed)) SWIM2_FPS;

static UB  sw_my_fp[8];
static INT sw_my_fp_ok;            /* 1 = sw_my_fp holds this node's key */
static UW  sw_keychg;              /* another key behind a known wide id */
static UW  sw_redraw_asked;        /* this node's own id heard with another (smaller) key */
static UB  sw_claim_said;          /* the "that key is larger" line, once */

static BOOL sw_fp_zero(const UB *f) { for (INT i = 0; i < 8; i++) if (f[i]) return FALSE; return TRUE; }
static INT  sw_fp_cmp(const UB *a, const UB *b)
{
    for (INT i = 0; i < 8; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
static void sw_puthex8(const UB *f)
{
    static const char H[] = "0123456789abcdef";
    char b[17];
    for (INT i = 0; i < 8; i++) { b[2 * i] = H[f[i] >> 4]; b[2 * i + 1] = H[f[i] & 15]; }
    b[16] = 0; sw_puts(b);
}
static const UB *sw_my_fp_get(void)
{
    if (!sw_my_fp_ok) {
        IMPORT int pk_node_fp(unsigned char fp[8]);   /* node_ident.c */
        if (pk_node_fp(sw_my_fp)) sw_my_fp_ok = 1;
    }
    return sw_my_fp_ok ? sw_my_fp : (const UB *)0;
}

/* This node's own wide id was heard with the key `f` at `now` (ms).
 * The larger key redraws at once. The smaller one waits: if it still hears the
 * other key SWIM2_CLAIM_WAIT_MS after it first did, the larger has not moved
 * (on the relay the news of the smaller key may not reach it: the relay's slot
 * and a third node's news both follow whoever spoke last, measured 11-21), so
 * it redraws itself. A larger key that moved is no longer heard here.
 * 11-21 (audit-208): but it is, for a while: the larger redrew at once and had
 * not yet spoken under its new id when the old key came past the 10 s (a third
 * node's pin, the 2nd and 4th news after a change), and both redrew. So the key
 * must come once past the 10 s and once more SWIM2_CLAIM_AGAIN_MS after that:
 * longer than a third node's pin of the moved key lasts (it gossips only ALIVE
 * ids, SUSPECT after SWIM_WIDE_SUSPECT_MS). Limit: the 2nd/4th news of several
 * third nodes, or of many ids (one news per id per few rounds), can span more. */
#define SWIM2_CLAIM_WAIT_MS 10000
#define SWIM2_CLAIM_AGAIN_MS (SWIM_WIDE_SUSPECT_MS + SWIM_PROBE_INTERVAL_MS)
static UB  sw_claim_seen;          /* the smaller side heard the other key (1), and past the 10 s (2) */
static UW  sw_claim_t0;            /* when it first did                      */
static UW  sw_claim_t1;            /* when it first did past the 10 s        */
static void sw_redraw_now(const UB *f, const UB *mine, const char *why)
{
    sw_redraw_asked++;
    if (sw_selftest_depth != 0) return;
    sw_puts("[swim] wide id "); sw_putdec(swim_wide_me);
    sw_puts(": another key "); sw_puthex8(f); sw_puts(" holds it, mine "); sw_puthex8(mine);
    sw_puts(why);
#ifndef N2C_NC_NO_REDRAW
    IMPORT void pk_node_wide_redraw(void);   /* node_ident.c: re-exec */
    pk_node_wide_redraw();
#endif
}
/* 11-21 (audit-207): keys heard under another wide id, straight or in the news.
 * A key is one node and a node is under one id, so such a key on my id is the
 * node that moved away; its old key still comes late (a third node's pin, the
 * 2nd and 4th news carry the previous key) and set off both redraws in CI
 * 44880448567 (the smaller's 10 s rule fired on the moved larger, and the other
 * way round). Filled before the claims of a packet are read (one packet may
 * carry both). Limit: whoever sends my key as news of another id keeps me from
 * redrawing on it; a redraw only avoids a clash, the clash stays as before N2c. */
#define SW_MOVED_N 16
static UB  sw_mv_fp[SW_MOVED_N][8];
static INT sw_mv_n, sw_mv_w;
static void sw_moved_note(const UB *f)
{
    if (sw_fp_zero(f)) return;
    for (INT i = 0; i < sw_mv_n; i++) if (sw_fp_cmp(sw_mv_fp[i], f) == 0) return;
    for (INT i = 0; i < 8; i++) sw_mv_fp[sw_mv_w][i] = f[i];
    sw_mv_w = (sw_mv_w + 1) % SW_MOVED_N;
    if (sw_mv_n < SW_MOVED_N) sw_mv_n++;
}
static BOOL sw_moved(const UB *f)
{
#if defined(N2C_NC_NO_MOVED)
    (void)f; return FALSE;   /* falsifier: a moved key is a claim as before (I6u) */
#else
    BOOL r = FALSE;
    PK_NOSW_ENTER();
    for (INT i = 0; i < sw_mv_n && !r; i++) if (sw_fp_cmp(sw_mv_fp[i], f) == 0) r = TRUE;
    for (INT i = 0; i < DREGION_MAX && !r; i++)
        if (wide_map.used[i] && wide_st[i].fp_ok && sw_fp_cmp(wide_st[i].fp, f) == 0) r = TRUE;
    PK_NOSW_LEAVE();
    return r;
#endif
}

static void swim_wide_claim(const UB *f, UW now)
{
    const UB *mine;
    if (!swim_wide_me || sw_fp_zero(f) || !(mine = sw_my_fp_get())) return;
    INT c = sw_fp_cmp(mine, f);
    if (c == 0) return;
    if (sw_moved(f)) return;
    if (c > 0) { sw_redraw_now(f, mine, " is the larger: redraw\r\n"); return; }
    if (sw_claim_seen == 2) {   /* heard past the 10 s: redraw when it comes again */
#ifndef N2C_NC_CLAIM_ONCE
        if ((UW)(now - sw_claim_t1) >= SWIM2_CLAIM_AGAIN_MS && (UW)(now - sw_claim_t1) < 0x80000000UL)
#endif
            sw_redraw_now(f, mine, " is the smaller, but the larger is still here after 10 s and 7 s: redraw\r\n");
        return;
    }
    if (!sw_claim_seen) {
        sw_claim_seen = 1; sw_claim_t0 = now;
        if (!sw_claim_said && sw_selftest_depth == 0) {
            sw_claim_said = 1;
            sw_puts("[swim] wide id "); sw_putdec(swim_wide_me);
            sw_puts(": another key "); sw_puthex8(f); sw_puts(" holds it; that key is the larger, it redraws\r\n");
        }
        return;
    }
#ifndef N2C_NC_SMALL_NEVER
    if ((UW)(now - sw_claim_t0) >= SWIM2_CLAIM_WAIT_MS && (UW)(now - sw_claim_t0) < 0x80000000UL) {
#if defined(N2C_NC_CLAIM_ONCE)   /* falsifier: one hearing past the 10 s redraws, as before (idpin) */
        sw_redraw_now(f, mine, " is the smaller, but the larger has not moved in 10 s: redraw\r\n");
#else
        sw_claim_seen = 2; sw_claim_t1 = now;
#endif
    }
#endif
}

/* direct: the packet came from this node. Otherwise it is news of the id.
 * fp: the sender's key from the trailer (direct only), or 0. */
static void swim_wide_heard(pk_node_t id, UW ip, BOOL direct, UW now, const UB *fp)
{
    if (!PK_NODE_IS_WIDE(id) || id == swim_wide_me) return;
    PK_NOSW_ENTER();      /* swim_rx (net_task) and swim_wide_tick (swim_task) */
    INT s = wide_add(id);
    if (s < 0) { PK_NOSW_LEAVE(); return; }
    SWIM_WIDE *e = &wide_st[s];
    UB old = e->state, chg = 0, was[8];
    if (direct && fp && !sw_fp_zero(fp)) {
        if (!e->fp_ok) {
            for (INT i = 0; i < 8; i++) e->fp[i] = fp[i];
            e->fp_ok = 1;
        } else if (sw_fp_cmp(e->fp, fp) != 0) {
#ifndef N2C_NC_NO_PIN
            /* another node under this id: nothing of the old one carries over */
            for (INT i = 0; i < 8; i++) { was[i] = e->fp[i]; e->fp_was[i] = e->fp[i]; e->fp[i] = fp[i]; }
            e->rtt_ok = 0; e->rtt_ms = 0; e->ping_seq = 0; e->ping_ms = 0;
#ifndef N2C_NC_NO_WAS
            e->was_left = 4;   /* the new holder of the relay slot must hear the old key */
#endif
            sw_keychg++; chg = 1;
#endif
        }
    }
    if (direct) { e->ip = ip; e->heard_ms = now; e->state = DNODE_ALIVE; }
    else if (old == DNODE_UNKNOWN) {
        e->ip = pk_node_ip(id); e->heard_ms = now; e->state = DNODE_ALIVE;
    }
    UB st = e->state;
    PK_NOSW_LEAVE();
    if (chg && sw_selftest_depth == 0) {
        sw_puts("[swim] wide node "); sw_putdec(id); sw_puts(": another key ");
        sw_puthex8(fp); sw_puts(" (was "); sw_puthex8(was); sw_puts("), a new node: its round trip dropped\r\n");
    }
    if (old != st) {
        sw_puts("[swim] wide node "); sw_putdec(id);
        sw_puts(old == DNODE_UNKNOWN ? " discovered\r\n" : " recovered\r\n");
    }
}

/* v2 round trip (N2b): a node that knows wide peers PINGs one of them per
 * round (type SWIM_PING, seq != 0, probe_target = the peer); the peer answers
 * SWIM_ACK with the same seq and probe_target = the asker. Both go to the
 * broadcast address like the beacon, so no wide address has to route. */
static void swim_wide_rtt_sample(pk_node_t id, UH seq, UW now)
{
    PK_NOSW_ENTER();
    INT s = nodemap_find(&wide_map, id);
    if (s != NODEMAP_NOSLOT && seq != 0 && wide_st[s].ping_seq == seq) {
        SWIM_WIDE *e = &wide_st[s];
        UW rtt = (UW)(now - e->ping_ms);
        if (rtt < 0x80000000UL) {          /* not an ACK from before its PING */
            e->rtt_ms = e->rtt_ok ? (e->rtt_ms * 3 + rtt + 2) / 4 : rtt;
            e->rtt_ok = 1;
            e->ping_seq = 0;               /* one PING, one sample */
        }
    }
    PK_NOSW_LEAVE();
}

/* N2b step (b2): a wide self cannot send a v1 PING (its id has no v1 form), so it
 * times its narrow peers with the v2 PING too; a new narrow node answers it (the
 * ACK path above), an old one cannot, so it stays unmeasured. The sample goes to
 * the v1 table (rtt_observe), which region_recompute reads for a narrow id.
 * -DN2_NC_WIDE_NO_NARROW_PING is the negative control of [n2-mix] XG. */
static void swim_wide_send(UB type, UH seq, pk_node_t me, pk_node_t target);
/* N3 step 1: SWIM_PEER.nping_seq / nping_ms (seq of the v2 PING in flight,
 * 0 = none, and when it went out); swim_nping_cur walks the slots */
static INT swim_nping_cur;

static void swim_wide_narrow_sample(pk_node_t id, UH seq, UW now)
{
    if (id >= DNODE_MAX || seq == 0) return;
    PK_NOSW_ENTER();
    SWIM_PEER *sp = sw_get(id);
    if (sp && sp->nping_seq == seq) {
        UW rtt = (UW)(now - sp->nping_ms);
        sp->nping_seq = 0;                         /* one PING, one sample */
        if (rtt < 0x80000000UL) rtt_observe(id, rtt);
    }
    PK_NOSW_LEAVE();
}

/* the round's PING to the next ALIVE narrow peer (a wide self only); 0 if none */
static UH swim_wide_ping_narrow(pk_node_t me)
{
    PK_NOSW_ENTER();
    pk_node_t target = 0xFF; UH seq = 0;
    for (INT k = 0; k < SW_STORE; k++) {
        swim_nping_cur = (swim_nping_cur + 1) % SW_STORE;
        if (!sw_map.used[swim_nping_cur]) continue;
        pk_node_t id = (pk_node_t)sw_map.id[swim_nping_cur];
        if (DROW(id).state != DNODE_ALIVE) continue;
        if (++swim_wide_seq == 0) swim_wide_seq = 1;
        seq = swim_wide_seq;
        sw_peer[swim_nping_cur].nping_seq = seq; sw_peer[swim_nping_cur].nping_ms = swim_wide_now();
        target = id;
        break;
    }
    PK_NOSW_LEAVE();
    if (seq) swim_wide_send(SWIM_PING, seq, me, target);
    return seq;
}

/* the smoothed round trip to a wide peer, 0xFFFFFFFF if not measured */
UW swim_wide_rtt(pk_node_t id)
{
    PK_NOSW_ENTER();
    INT s = nodemap_find(&wide_map, id);
    UW r = (s != NODEMAP_NOSLOT && wide_st[s].rtt_ok) ? wide_st[s].rtt_ms : 0xFFFFFFFFUL;
    PK_NOSW_LEAVE();
    return r;
}

INT swim_wide_alive(pk_node_t *out, INT max)
{
    INT n = 0;
    PK_NOSW_ENTER();
    for (INT i = 0; i < DREGION_MAX && n < max; i++)
        if (wide_map.used[i] && wide_st[i].state == DNODE_ALIVE)
            out[n++] = (pk_node_t)wide_map.id[i];
    PK_NOSW_LEAVE();
    return n;
}

#if defined(DNODE_ROWS_IN_SLOTS)
/* N3 step 3 (research/n3-gl-region.md §2 3): the narrow ids that have a row
 * (a slot whose row is not UNKNOWN), up to max, into out; returns the count.
 * O(SW_STORE), not O(DNODE_MAX): gossip_learn walks these instead of every id. */
INT swim_row_ids(pk_node_t *out, INT max)
{
    INT n = 0;
    PK_NOSW_ENTER();
    for (INT i = 0; i < SW_STORE && n < max; i++)
        if (sw_map.used[i] && sw_peer[i].row.state != DNODE_UNKNOWN)
            out[n++] = (pk_node_t)sw_map.id[i];
    PK_NOSW_LEAVE();
    return n;
}
#endif

static pk_node_t swim_wide_self_id(void)
{
    return swim_wide_me ? swim_wide_me : drpc_my_node;   /* 0xFF = none yet */
}

/* the self-test catches what swim_wide_send would send (audit-122: only the
 * receiving side of the ACK was tested). Only sends from the task that set the
 * tap are caught (audit-124 limit 2: the swim task's own PING during the test
 * would otherwise be swallowed). */
static SWIM2_PKT *swim_wide_tap;
static ID         swim_wide_tap_tid;

static void swim_wide_send(UB type, UH seq, pk_node_t me, pk_node_t target)
{
    SWIM2_PKT b;
    UB *pb = (UB *)&b;
    for (INT i = 0; i < (INT)sizeof b; i++) pb[i] = 0;
    b.magic = SWIM_MAGIC; b.version = SWIM2_VERSION; b.type = type;
    b.seq = seq; b.src_node = me; b.probe_target = target;
    if (swim_wide_tap && tk_get_tid() == swim_wide_tap_tid) { *swim_wide_tap = b; return; }
    udp_send(IP4(255, 255, 255, 255), SWIM_PORT, SWIM_PORT,
             (const UB *)&b, (UH)sizeof(b));
}

/* 1 = this was a v2 packet (used or dropped), 0 = not ours. */
static INT swim_wide_rx(UW src_ip, const UB *data, UH len)
{
    const SWIM2_PKT *p = (const SWIM2_PKT *)data;
    if (len < 5 || p->magic != SWIM_MAGIC || p->version != SWIM2_VERSION) return 0;
    if (len < (UH)sizeof(SWIM2_PKT)) { swim2_bad++; return 1; }
    UW now = swim_wide_now();
    /* N2c stage 3: the keys after the packet, if any (an old sender has none) */
    const SWIM2_FPS *fx = 0;
    INT fn = 0;
    if (len >= (UH)(sizeof(SWIM2_PKT) + 4)) {
        const SWIM2_FPS *t = (const SWIM2_FPS *)(data + sizeof(SWIM2_PKT));
        if (t->tag[0] == 'N' && t->tag[1] == '2' && t->tag[2] == 'F') {
            fn = t->n;
            if (fn > SWIM2_FP_N) fn = SWIM2_FP_N;
            if (fn > (INT)((len - sizeof(SWIM2_PKT) - 4) / 8)) fn = (INT)((len - sizeof(SWIM2_PKT) - 4) / 8);
            fx = t;
        }
    }
    if (fx && swim_wide_me) {   /* keys under other wide ids first: they are moved keys on mine */
        PK_NOSW_ENTER();
        if (fn > 0 && p->src_node != swim_wide_me && PK_NODE_IS_WIDE(p->src_node)) sw_moved_note(fx->fp[0]);
        for (INT i = 0; i < p->gossip_cnt && i < SWIM_GOSSIP_MAX && 1 + i < fn; i++)
            if (p->gossip[i].node_id != swim_wide_me && PK_NODE_IS_WIDE(p->gossip[i].node_id)
                && p->gossip[i].state == DNODE_ALIVE)
                sw_moved_note(fx->fp[1 + i]);
        PK_NOSW_LEAVE();
    }
    if (fx && fn > 0 && p->src_node == swim_wide_me) swim_wide_claim(fx->fp[0], now);   /* a LAN hears itself */
    swim_wide_heard(p->src_node, src_ip, TRUE, now, (fx && fn > 0) ? fx->fp[0] : (const UB *)0);
    for (INT i = 0; i < p->gossip_cnt && i < SWIM_GOSSIP_MAX; i++) {
        if (fx && 1 + i < fn && swim_wide_me && p->gossip[i].node_id == swim_wide_me)
            swim_wide_claim(fx->fp[1 + i], now);
        if (p->gossip[i].state == DNODE_ALIVE)
            swim_wide_heard(p->gossip[i].node_id, 0, FALSE, now, (const UB *)0);
    }
    pk_node_t me = swim_wide_self_id();
    if (me != 0xFF && p->seq != 0 && p->probe_target == me) {
        if (p->type == SWIM_PING)     swim_wide_send(SWIM_ACK, p->seq, me, p->src_node);
        else if (p->type == SWIM_ACK) {
            if (p->src_node < DNODE_MAX) swim_wide_narrow_sample(p->src_node, p->seq, now);
            else                         swim_wide_rtt_sample(p->src_node, p->seq, now);
        }
    }
    return 1;
}

/* Fill up to SWIM_GOSSIP_MAX live wide ids, taking turns. Returns the count.
 * fx (or 0): the trailer; fp[1 + i] gets the key kept for gossip[i]. */
static INT swim_wide_fill(SWIM2_PKT *p, SWIM2_FPS *fx)
{
    INT n = 0;
    for (INT k = 0; k < DREGION_MAX && n < SWIM_GOSSIP_MAX; k++) {
        swim_wide_cursor = (swim_wide_cursor + 1) % DREGION_MAX;
        if (!wide_map.used[swim_wide_cursor]) continue;
        if (wide_st[swim_wide_cursor].state != DNODE_ALIVE) continue;
        p->gossip[n].node_id     = (UH)wide_map.id[swim_wide_cursor];
        p->gossip[n].state       = DNODE_ALIVE;
        p->gossip[n].incarnation = 0;
        p->gossip[n].capability  = 0;
        if (fx && wide_st[swim_wide_cursor].fp_ok) {
            /* after a key change the 1st and 3rd news carry the previous key: on the
             * relay this id's slot follows whoever spoke last, as the kept key does,
             * so without it the news reaches only that key's own holder. 11-23: it
             * was the 2nd and 4th, which never went out while both keep talking
             * (each change starts the count again; idpin "two keys heard in turn") */
            SWIM_WIDE *e = &wide_st[swim_wide_cursor];
            const UB *k = e->fp;
#ifdef N2C_NC_WAS_SECOND   /* falsifier: the previous key in the 2nd and 4th news, as before (idpin) */
            if (e->was_left) { e->was_left--; if (!(e->was_left & 1)) k = e->fp_was; }
#else
            if (e->was_left) { e->was_left--; if (e->was_left & 1) k = e->fp_was; }
#endif
            for (INT i = 0; i < 8; i++) fx->fp[1 + n][i] = k[i];
        }
        n++;
    }
    return n;
}

static void swim_wide_age(UW now)
{
    for (INT i = 0; i < DREGION_MAX; i++) {
        SWIM_WIDE *e = &wide_st[i];
        PK_NOSW_ENTER();
        UB old = e->state, st = old;
        if (wide_map.used[i] && old != DNODE_DEAD && old != DNODE_UNKNOWN) {
            UW age = (UW)(now - e->heard_ms);
            if (age >= 0x80000000UL) age = 0;   /* heard after now was read */
            st = age >= SWIM_WIDE_DEAD_MS ? DNODE_DEAD
               : age >= SWIM_WIDE_SUSPECT_MS ? DNODE_SUSPECT : DNODE_ALIVE;
#ifdef N2_NC_WIDE_NEVER_DEAD   /* falsifier: [n2-alive] A3 and A4 (a wide peer never dies) */
            if (st == DNODE_DEAD) st = DNODE_SUSPECT;
#endif
            e->state = st;
        }
        UW id = wide_map.id[i];
        PK_NOSW_LEAVE();
        if (st != old) {
            sw_puts("[swim] wide node "); sw_putdec(id);
            sw_puts(st == DNODE_DEAD ? " -> DEAD\r\n"
                  : st == DNODE_SUSPECT ? " -> SUSPECT\r\n" : " -> ALIVE\r\n");
        }
    }
}

static UH swim_wide_ping_next(pk_node_t me);

static void swim_wide_tick(void)
{
    swim_wide_age(swim_wide_now());
    PK_NOSW_ENTER();
    static UB out[sizeof(SWIM2_PKT) + sizeof(SWIM2_FPS)];   /* swim_task only */
    SWIM2_PKT b;
    SWIM2_FPS fx;
    UB *pb = (UB *)&b;
    for (INT i = 0; i < (INT)sizeof b; i++) pb[i] = 0;
    pb = (UB *)&fx;
    for (INT i = 0; i < (INT)sizeof fx; i++) pb[i] = 0;
    INT n = swim_wide_fill(&b, &fx);
    PK_NOSW_LEAVE();
    /* A node that knows no wide id sends no v2 packet at all. */
    if (swim_wide_me == 0 && (n == 0 || drpc_my_node == 0xFF)) return;
    b.magic        = SWIM_MAGIC;
    b.version      = SWIM2_VERSION;
    b.type         = SWIM_ACK;     /* seq 0: a beacon, as in v1 */
    b.src_node     = swim_wide_me ? swim_wide_me : drpc_my_node;
    b.probe_target = b.src_node;
    b.gossip_cnt   = (UB)n;
    /* N2c stage 3: the trailer of keys (an old node reads the 29 bytes only) */
    fx.tag[0] = 'N'; fx.tag[1] = '2'; fx.tag[2] = 'F';
    fx.n = (UB)(1 + n);
    if (swim_wide_me) {
        const UB *mine = sw_my_fp_get();
        if (mine) for (INT i = 0; i < 8; i++) fx.fp[0][i] = mine[i];
    }
    for (UW i = 0; i < sizeof b; i++) out[i] = ((const UB *)&b)[i];
    for (UW i = 0; i < 4 + 8u * (UW)(1 + n); i++) out[sizeof b + i] = ((const UB *)&fx)[i];
    udp_send(IP4(255, 255, 255, 255), SWIM_PORT, SWIM_PORT,
             out, (UH)(sizeof(b) + 4 + 8 * (1 + n)));

    /* one v2 PING per round, to the next live wide peer */
    pk_node_t me = swim_wide_self_id();
    if (me == 0xFF) return;
    (void)swim_wide_ping_next(me);
#ifndef N2_NC_WIDE_NO_NARROW_PING
    if (swim_wide_me) (void)swim_wide_ping_narrow(me);   /* and one to a narrow peer */
#endif
}

/* the round's PING: the next live wide peer gets a fresh seq (never 0); the
 * self-test drives this to see that rounds are told apart (audit-124 M3).
 * Returns the seq sent, 0 if no peer is live. */
static UH swim_wide_ping_next(pk_node_t me)
{
    PK_NOSW_ENTER();
    pk_node_t target = 0xFF; UH seq = 0;
    for (INT k = 0; k < DREGION_MAX; k++) {
        swim_wide_pcur = (swim_wide_pcur + 1) % DREGION_MAX;
        SWIM_WIDE *e = &wide_st[swim_wide_pcur];
        if (!wide_map.used[swim_wide_pcur] || e->state != DNODE_ALIVE) continue;
        if (++swim_wide_seq == 0) swim_wide_seq = 1;
        seq = swim_wide_seq;
        e->ping_seq = seq; e->ping_ms = swim_wide_now();
        target = (pk_node_t)wide_map.id[swim_wide_pcur];
        break;
    }
    PK_NOSW_LEAVE();
    if (seq) swim_wide_send(SWIM_PING, seq, me, target);
    return seq;
}

static void swim_wide_print(void)
{
    if (swim_wide_me) {
        sw_puts("            "); sw_putdec(swim_wide_me);
        sw_puts("  SELF     "); sw_puts(ip_str(NET_MY_IP)); sw_puts("\r\n");
    }
    for (INT i = 0; i < DREGION_MAX; i++) {
        const SWIM_WIDE *e = &wide_st[i];
        if (!wide_map.used[i] || e->state == DNODE_UNKNOWN) continue;
        sw_puts("            "); sw_putdec(wide_map.id[i]);
        sw_puts("  "); sw_puts(state_str(e->state));
        sw_puts("  "); sw_puts(ip_str(e->ip));
        sw_puts("  wide");
        if (e->rtt_ok) { sw_puts("  rtt="); sw_putdec(e->rtt_ms); sw_puts("ms"); }
        sw_puts("\r\n");
    }
}

/* `nodes wide`: drives swim_wide_rx and the aging with crafted packets; the
 * table is saved and put back, so a live node can run it. */
static void sw2_build(SWIM2_PKT *p, UH src, UH g0, UB st, UB cnt)
{
    UB *pb = (UB *)p;
    for (INT i = 0; i < (INT)sizeof *p; i++) pb[i] = 0;
    p->magic = SWIM_MAGIC; p->version = SWIM2_VERSION; p->type = SWIM_ACK;
    p->src_node = src; p->probe_target = src;
    p->gossip_cnt = cnt;
    for (INT i = 0; i < SWIM_GOSSIP_MAX; i++) { p->gossip[i].node_id = 0xFFFF; }
    p->gossip[0].node_id = g0; p->gossip[0].state = st;
}

static INT sw2_find(pk_node_t id) { return nodemap_find(&wide_map, id); }

static INT sw2_state(pk_node_t id)
{
    INT s = sw2_find(id);
    return s < 0 ? -1 : (INT)wide_st[s].state;
}

INT swim_wide_self_test(void (*emit)(const char *))
{
    SW_SELFTEST_ENTER();
    static SWIM_WIDE saved[DREGION_MAX];
    static NODEMAP   saved_map;
    PK_NOSW_ENTER();
    saved_map = wide_map;
    for (INT i = 0; i < DREGION_MAX; i++) saved[i] = wide_st[i];
    nodemap_init(&wide_map, 0);
    pk_node_t sv_me = swim_wide_me; UW sv_full = nm_full, sv_bad = swim2_bad;
    PK_NOSW_LEAVE();
    INT fails = 0;
#define W2(cond, what) do { if (!(cond)) { fails++; \
        emit("[swim-wide] FAIL "); emit(what); emit("\r\n"); } } while (0)
    SWIM2_PKT p;
    UW ip = pk_node_ip(299);

    /* A v1 packet is not ours; a short v2 packet is ours and dropped. */
    sw2_build(&p, 299, 0xFFFF, DNODE_ALIVE, 0);
    p.version = SWIM_VERSION;
    W2(swim_wide_rx(ip, (const UB *)&p, sizeof p) == 0, "v1 is left to swim_rx");
    p.version = SWIM2_VERSION;
    W2(swim_wide_rx(ip, (const UB *)&p, sizeof p - 1) == 1, "short v2 is taken");
    W2(swim2_bad == sv_bad + 1, "short v2 is counted");
    W2(sw2_find(299) < 0, "short v2 adds nobody");

    /* A wide sender is known, under its own number. */
    W2(swim_wide_rx(ip, (const UB *)&p, sizeof p) == 1, "v2 is taken");
    W2(sw2_state(299) == DNODE_ALIVE, "wide sender is ALIVE");
    W2(wide_st[sw2_find(299)].ip == ip, "wide sender's ip kept");
    W2(ip != pk_node_ip(43), "300 and 44 have different addresses");
    W2(pk_node_ip(43) == (((UW)44 << 24) | 0x0000010AUL), "a narrow address is unchanged");

    /* A one-byte field: a narrow id as it is, a wide id is not a node, never its
     * low byte. No path writes a wide id there today, so only this looks at it. */
    W2(pk_node_v1(43) == 43, "pk_node_v1 keeps a narrow id");
    W2(pk_node_v1(299) == PK_NODE_NONE, "pk_node_v1 does not cut a wide id");
    W2(pk_node_v1(0xFFFF) == PK_NODE_NONE, "pk_node_v1 of 0xFFFF is not a node");

    /* Not wide: never in the table. 0xFF and 0xFFFF are not nodes. */
    sw2_build(&p, 43, 0xFFFF, DNODE_ALIVE, 1);
    swim_wide_rx(ip, (const UB *)&p, sizeof p);
    W2(sw2_find(43) < 0, "a narrow sender stays out");
    W2(sw2_find(0xFFFF) < 0, "0xFFFF stays out");
    sw2_build(&p, 0xFF, 0xFF, DNODE_ALIVE, 1);
    swim_wide_rx(ip, (const UB *)&p, sizeof p);
    W2(sw2_find(0xFF) < 0, "0xFF stays out");

    /* News introduces an id, at its own address. */
    sw2_build(&p, 43, 700, DNODE_ALIVE, 1);
    swim_wide_rx(ip, (const UB *)&p, sizeof p);
    W2(sw2_state(700) == DNODE_ALIVE, "news introduces a wide id");
    W2(wide_st[sw2_find(700)].ip == pk_node_ip(700), "at its own address");

    /* A bad count reads no further than the array: what follows the packet
     * looks like one more live entry, and must stay unread. */
    {
        static UB big[sizeof(SWIM2_PKT) + sizeof(SWIM2_GOSSIP_EVT)];
        SWIM2_GOSSIP_EVT over = { 702, DNODE_ALIVE, 0, 0 };
        sw2_build(&p, 43, 701, DNODE_ALIVE, 200);
        for (UW i = 0; i < sizeof p; i++) big[i] = ((const UB *)&p)[i];
        for (UW i = 0; i < sizeof over; i++) big[sizeof p + i] = ((const UB *)&over)[i];
        swim_wide_rx(ip, big, (UH)sizeof big);
        W2(sw2_state(701) == DNODE_ALIVE, "a count of 200 is read as the maximum");
        W2(sw2_find(702) < 0, "nothing past the array is read");
    }

    /* A rumour of a dead or suspect stranger introduces nobody. */
    sw2_build(&p, 43, 703, DNODE_DEAD, 1);
    swim_wide_rx(ip, (const UB *)&p, sizeof p);
    W2(sw2_find(703) < 0, "a DEAD rumour introduces nobody");

    /* A beacon can land after the tick read its clock and before the tick
     * reached this slot: heard_ms ahead of now is not an age of 49 days. */
    INT s = sw2_find(299);
    swim_wide_age(wide_st[s].heard_ms - 1);
    W2(wide_st[s].state == DNODE_ALIVE, "a beacon newer than the tick's clock is not old");

    /* That reading of the clock must not eat real ages: the half of the 32-bit
     * range is the line, and a stamp across the wrap ages like any other. */
    {
        UW keep = wide_st[s].heard_ms;
        wide_st[s].heard_ms = 1000; swim_wide_age(1000UL + 0x80000000UL);
        W2(wide_st[s].state == DNODE_ALIVE, "half the clock's range reads as heard after now");
        wide_st[s].heard_ms = 1000; swim_wide_age(1000UL + 0x7FFFFFFFUL);
        W2(wide_st[s].state == DNODE_DEAD, "one less is old");
        wide_st[s].state = DNODE_ALIVE;
        wide_st[s].heard_ms = 1000; swim_wide_age(1000UL + 0x10000UL);
        W2(wide_st[s].state == DNODE_DEAD, "a late tick still sees 65 s as old");
        wide_st[s].state = DNODE_ALIVE;
        wide_st[s].heard_ms = 0xFFFFFFF0UL; swim_wide_age(10);
        W2(wide_st[s].state == DNODE_ALIVE, "26 ms across the wrap is young");
        wide_st[s].heard_ms = 0xFFFFFFF0UL; swim_wide_age((UW)(0xFFFFFFF0UL + SWIM_WIDE_DEAD_MS));
        W2(wide_st[s].state == DNODE_DEAD, "15 s across the wrap is old");
        wide_st[s].state = DNODE_ALIVE; wide_st[s].heard_ms = keep;
    }

    /* A rumour neither kills nor revives; aging does, and a beacon revives. */
    sw2_build(&p, 43, 299, DNODE_DEAD, 1);
    swim_wide_rx(ip, (const UB *)&p, sizeof p);
    W2(wide_st[s].state == DNODE_ALIVE, "a DEAD rumour does not kill");
    UW t0 = wide_st[s].heard_ms;
    swim_wide_age(t0 + SWIM_WIDE_SUSPECT_MS - 1);
    W2(wide_st[s].state == DNODE_ALIVE, "alive just before the suspect line");
    swim_wide_age(t0 + SWIM_WIDE_SUSPECT_MS);
    W2(wide_st[s].state == DNODE_SUSPECT, "suspect at the line");
    sw2_build(&p, 43, 299, DNODE_ALIVE, 1);
    swim_wide_rx(ip, (const UB *)&p, sizeof p);
    W2(wide_st[s].state == DNODE_SUSPECT, "an ALIVE rumour does not revive");
    swim_wide_age(t0 + SWIM_WIDE_DEAD_MS);
    W2(wide_st[s].state == DNODE_DEAD, "dead at the line");
    sw2_build(&p, 299, 0xFFFF, DNODE_ALIVE, 0);
    swim_wide_rx(ip, (const UB *)&p, sizeof p);
    W2(wide_st[s].state == DNODE_ALIVE, "its own beacon revives");

    /* Round trip: an ACK with the PING's seq, addressed to me, is a sample. */
    {
        /* at boot the clock can still read 0 (CI types nodes wide at once): a
         * PING timed 0 would pass the round check below (audit-126 m9). Wait
         * it off 0 here, before swim_wide_me is faked, so no real round runs
         * with the fake ids while this task sleeps. */
        for (INT w = 0; w < 50 && swim_wide_now() == 0; w++) tk_dly_tsk(2);
        SWIM_WIDE *e = &wide_st[s];
        e->ping_seq = 77; e->ping_ms = 5000; e->rtt_ok = 0;
        swim_wide_rtt_sample(299, 76, 5009);
        W2(!e->rtt_ok, "an ACK with another seq is no sample");
        swim_wide_rtt_sample(299, 77, 5009);
        W2(e->rtt_ok && e->rtt_ms == 9, "the matching ACK gives 9 ms");
        W2(swim_wide_rtt(299) == 9, "swim_wide_rtt reads it");
        W2(swim_rtt_ms(299) == 9, "swim_rtt_ms reads a wide peer");
        swim_wide_rtt_sample(299, 77, 5100);
        W2(e->rtt_ms == 9, "one PING gives one sample");
        e->ping_seq = 78; e->ping_ms = 6000;
        swim_wide_rtt_sample(299, 78, 6021);
        W2(e->rtt_ms == (9 * 3 + 21 + 2) / 4, "samples are smoothed as in v1");
        e->ping_seq = 79; e->ping_ms = 7000;
        swim_wide_rtt_sample(299, 79, 6990);
        W2(e->rtt_ms == (9 * 3 + 21 + 2) / 4, "an ACK before its PING is no sample");
        W2(swim_wide_rtt(0x5555) == 0xFFFFFFFFUL, "a stranger is not measured");
        W2(swim_rtt_ms(0x5555) == 0xFFFFFFFFUL, "nor through swim_rtt_ms");
        /* through the receive path: only an ACK to me with seq != 0 */
        pk_node_t me_keep = swim_wide_me; swim_wide_me = 810;
        e->ping_seq = 80; e->rtt_ok = 0;
        e->ping_ms = swim_wide_now();
        sw2_build(&p, 299, 0xFFFF, DNODE_ALIVE, 0);
        p.seq = 80; p.probe_target = 811;
        swim_wide_rx(ip, (const UB *)&p, sizeof p);
        W2(!e->rtt_ok, "an ACK to someone else is no sample");
        p.seq = 0; p.probe_target = 810;
        swim_wide_rx(ip, (const UB *)&p, sizeof p);
        W2(!e->rtt_ok, "a beacon (seq 0) is no sample");
        p.type = SWIM_PING; p.seq = 80;
        swim_wide_rx(ip, (const UB *)&p, sizeof p);
        W2(!e->rtt_ok, "a PING is no sample");
        p.type = SWIM_ACK;
        swim_wide_rx(ip, (const UB *)&p, sizeof p);
        W2(e->rtt_ok && e->rtt_ms < 1000, "the ACK to me is");
        /* the sending side: what a PING makes me answer (caught, not sent) */
        SWIM2_PKT got;
        UB *gb = (UB *)&got;
        for (INT i = 0; i < (INT)sizeof got; i++) gb[i] = 0;
        swim_wide_tap_tid = tk_get_tid();
        swim_wide_tap = &got;
        sw2_build(&p, 299, 0xFFFF, DNODE_ALIVE, 0);
        p.type = SWIM_PING; p.seq = 81; p.probe_target = 811;
        swim_wide_rx(ip, (const UB *)&p, sizeof p);
        W2(got.type == 0, "a PING to someone else is not answered");
        p.probe_target = 810;
        swim_wide_rx(ip, (const UB *)&p, sizeof p);
        swim_wide_tap = NULL;
        W2(got.type == SWIM_ACK && got.seq == 81, "a PING to me is answered by an ACK with its seq");
        W2(got.src_node == 810 && got.probe_target == 299, "the ACK goes from me back to the asker");
        /* rounds are told apart (audit-124 M3: a PING seq stuck at 1 was green):
         * two rounds to the one live peer carry two seqs, and the late ACK of
         * the first round is no sample after the second PING went out */
        for (INT i = 0; i < DREGION_MAX; i++)
            if (wide_map.used[i] && i != s) wide_st[i].state = DNODE_DEAD;
        e->rtt_ok = 0;
        SWIM2_PKT g1;
        swim_wide_tap = &got;
        for (INT i = 0; i < (INT)sizeof got; i++) gb[i] = 0;
        UH q1 = swim_wide_ping_next(810);
        g1 = got;
        for (INT i = 0; i < (INT)sizeof got; i++) gb[i] = 0;
        /* the round also renews the PING's time (audit-125 m7: a time kept
         * from an earlier round was green, the RTT would grow every round) */
        e->ping_ms = swim_wide_now() - 60000UL;
        UW t0 = swim_wide_now();
        W2(t0 != 0, "the clock has left 0 before the PING is timed");
        UH q2 = swim_wide_ping_next(810);
        UW t1 = swim_wide_now();
        swim_wide_tap = NULL;
        W2((UW)(e->ping_ms - t0) <= (UW)(t1 - t0), "the next round's PING is timed when it is sent");
        W2(q1 != 0 && g1.type == SWIM_PING && g1.seq == q1 && g1.src_node == 810
           && g1.probe_target == 299, "a round sends a PING with its seq to the live peer");
        W2(q2 != 0 && q2 != q1 && got.seq == q2 && got.probe_target == 299,
           "the next round's PING has another seq");
        swim_wide_rtt_sample(299, q1, e->ping_ms + 5);
        W2(!e->rtt_ok, "the late ACK of the earlier round is no sample");
        swim_wide_rtt_sample(299, q2, e->ping_ms + 5);
        W2(e->rtt_ok && e->rtt_ms == 5, "the ACK of this round is");
        /* step (b2): a narrow peer's ACK to a wide self's v2 PING is a v1 sample;
         * row 43's live entry is saved and put back */
        {
            SWIM_PEER *r43 = sw_tslot(43);
            UB kv = r43->rtt_valid; UW ke = r43->rtt_ewma_ms;
            UH ks = r43->nping_seq; UW km = r43->nping_ms;
            r43->rtt_valid = 0;
            r43->nping_seq = 90; r43->nping_ms = 1000;
            swim_wide_narrow_sample(43, 89, 1012);
            W2(!r43->rtt_valid, "a narrow ACK with another seq is no sample");
            swim_wide_narrow_sample(43, 90, 1012);
            W2(r43->rtt_valid && r43->rtt_ewma_ms == 12, "the narrow ACK of the PING gives 12 ms");
            swim_wide_narrow_sample(43, 90, 1100);
            W2(r43->rtt_ewma_ms == 12, "one narrow PING gives one sample");
            r43->rtt_valid = 0;
            r43->nping_seq = 91; r43->nping_ms = swim_wide_now();
            sw2_build(&p, 43, 0xFFFF, DNODE_ALIVE, 0);
            p.seq = 91; p.probe_target = 810;
            swim_wide_rx(pk_node_ip(43), (const UB *)&p, sizeof p);
            W2(r43->rtt_valid && r43->rtt_ewma_ms < 1000, "through the receive path: an ACK from 43 to me");
            W2(sw2_find(43) < 0, "and 43 is still not a wide peer");
            r43->rtt_valid = kv; r43->rtt_ewma_ms = ke;
            r43->nping_seq = ks; r43->nping_ms = km;
        }
        swim_wide_me = me_keep;
    }

    /* Own id: not a peer. */
    swim_wide_me = 800;
    sw2_build(&p, 800, 800, DNODE_ALIVE, 1);
    swim_wide_rx(pk_node_ip(800), (const UB *)&p, sizeof p);
    W2(sw2_find(800) < 0, "own id is not a peer");
    swim_wide_me = sv_me;

    /* Full: refused and counted; a dead entry makes room. */
    nodemap_init(&wide_map, 0);
    for (INT i = 0; i < DREGION_MAX; i++) {
        sw2_build(&p, (UH)(1000 + i), 0xFFFF, DNODE_ALIVE, 0);
        swim_wide_rx(ip, (const UB *)&p, sizeof p);
    }
    W2(sw2_find((pk_node_t)(1000 + DREGION_MAX - 1)) >= 0, "the table holds DREGION_MAX");
    sw2_build(&p, 2000, 0xFFFF, DNODE_ALIVE, 0);
    swim_wide_rx(ip, (const UB *)&p, sizeof p);
    W2(sw2_find(2000) < 0 && nm_full == sv_full + 1, "the next id is refused and counted");
    wide_st[5].state = DNODE_DEAD;
    swim_wide_rx(ip, (const UB *)&p, sizeof p);
    W2(sw2_find(2000) == 5, "a dead slot is reused");

    /* What goes out: only live ids, taking turns. */
    nodemap_init(&wide_map, 0);
    wide_st[nodemap_admit(&wide_map, 900)].state = DNODE_ALIVE;
    wide_st[nodemap_admit(&wide_map, 901)].state = DNODE_DEAD;
    { SWIM2_PKT o; W2(swim_wide_fill(&o, 0) == 1 && o.gossip[0].node_id == 900, "only live ids go out"); }

    PK_NOSW_ENTER();
    wide_map = saved_map;
    for (INT i = 0; i < DREGION_MAX; i++) wide_st[i] = saved[i];
    nm_full = sv_full; swim2_bad = sv_bad;
    PK_NOSW_LEAVE();
#undef W2
    emit(fails ? "[swim-wide] FAIL\r\n" : "[swim-wide] PASS\r\n");
    SW_SELFTEST_LEAVE();
    return fails;
}

/* `nodes idpin` (N2c stage 3, I6u): the keys after a v2 packet, on a fresh wide
 * table, saved and put back as swim_wide_self_test does. Nothing is sent and no
 * redraw runs (sw_selftest_depth > 0 only counts the ask). */
static UH sw3_pkt(UB *buf, UH src, UH g0, const UB *f0, const UB *fg)
{
    SWIM2_PKT p;
    SWIM2_FPS t;
    UB *pt = (UB *)&t;
    sw2_build(&p, src, g0, DNODE_ALIVE, g0 == 0xFFFF ? 0 : 1);
    for (UW i = 0; i < sizeof t; i++) pt[i] = 0;
    t.tag[0] = 'N'; t.tag[1] = '2'; t.tag[2] = 'F'; t.n = 2;
    if (f0) for (INT i = 0; i < 8; i++) t.fp[0][i] = f0[i];
    if (fg) for (INT i = 0; i < 8; i++) t.fp[1][i] = fg[i];
    for (UW i = 0; i < sizeof p; i++) buf[i] = ((const UB *)&p)[i];
    for (UW i = 0; i < 4 + 16; i++) buf[sizeof p + i] = pt[i];
    return (UH)(sizeof p + 4 + 16);
}

INT swim_idpin_self_test(void (*emit)(const char *))
{
    SW_SELFTEST_ENTER();
    static SWIM_WIDE saved[DREGION_MAX];
    static NODEMAP   saved_map;
    static UB        buf[sizeof(SWIM2_PKT) + sizeof(SWIM2_FPS)];
    PK_NOSW_ENTER();
    saved_map = wide_map;
    for (INT i = 0; i < DREGION_MAX; i++) saved[i] = wide_st[i];
    nodemap_init(&wide_map, 0);
    pk_node_t sv_me = swim_wide_me;
    UB sv_fp[8]; INT sv_fp_ok = sw_my_fp_ok;
    for (INT i = 0; i < 8; i++) sv_fp[i] = sw_my_fp[i];
    UW sv_chg = sw_keychg, sv_ask = sw_redraw_asked;
    UB sv_seen = sw_claim_seen; UW sv_t0 = sw_claim_t0, sv_t1 = sw_claim_t1;
    sw_claim_seen = 0;
    static UB sv_mv[SW_MOVED_N][8];
    INT sv_mv_n = sw_mv_n, sv_mv_w = sw_mv_w;
    for (INT i = 0; i < SW_MOVED_N; i++) for (INT j = 0; j < 8; j++) sv_mv[i][j] = sw_mv_fp[i][j];
    sw_mv_n = 0; sw_mv_w = 0;
    PK_NOSW_LEAVE();
    INT fails = 0;
#define I6(cond, what) do { if (!(cond)) { fails++; \
        emit("[idpin] FAIL "); emit(what); emit("\r\n"); } } while (0)
    static const UB X[8] = { 0x11, 1, 2, 3, 4, 5, 6, 7 };
    static const UB Y[8] = { 0x22, 1, 2, 3, 4, 5, 6, 7 };
    static const UB L[8] = { 0x40, 0, 0, 0, 0, 0, 0, 1 };
    static const UB M[8] = { 0x80, 0, 0, 0, 0, 0, 0, 0 };
    static const UB H[8] = { 0xC0, 0, 0, 0, 0, 0, 0, 0 };
    UW ip = pk_node_ip(299);
    UH n;
    INT s;

    /* the first key heard from a wide id is kept */
    n = sw3_pkt(buf, 299, 0xFFFF, X, 0);
    I6(swim_wide_rx(ip, buf, n) == 1, "a v2 packet with keys is taken");
    s = sw2_find(299);
    I6(s >= 0 && wide_st[s].fp_ok && sw_fp_cmp(wide_st[s].fp, X) == 0, "the first key is kept");
    /* the same key changes nothing */
    if (s >= 0) { wide_st[s].rtt_ok = 1; wide_st[s].rtt_ms = 50; }
    swim_wide_rx(ip, buf, n);
    I6(s >= 0 && wide_st[s].rtt_ok == 1 && wide_st[s].rtt_ms == 50 && sw_keychg == sv_chg,
       "the same key keeps the round trip");
    /* a packet without keys (an old sender) changes nothing */
    {
        SWIM2_PKT p; sw2_build(&p, 299, 0xFFFF, DNODE_ALIVE, 0);
        swim_wide_rx(ip, (const UB *)&p, sizeof p);
        I6(s >= 0 && sw_fp_cmp(wide_st[s].fp, X) == 0 && wide_st[s].rtt_ok == 1, "no keys: nothing changes");
    }
    /* another key is another node: the round trip is dropped, the new key kept */
    n = sw3_pkt(buf, 299, 0xFFFF, Y, 0);
    swim_wide_rx(ip, buf, n);
    I6(s >= 0 && sw_fp_cmp(wide_st[s].fp, Y) == 0, "another key is kept as the new one");
    I6(s >= 0 && wide_st[s].rtt_ok == 0 && wide_st[s].ping_seq == 0, "another key drops the round trip");
    I6(sw_keychg == sv_chg + 1, "another key is counted once");
    I6(s >= 0 && wide_st[s].state == DNODE_ALIVE, "the new node is ALIVE");
    /* news never sets a key: only the id's own beacon does */
    n = sw3_pkt(buf, 43, 700, 0, X);
    swim_wide_rx(ip, buf, n);
    I6(sw2_state(700) == DNODE_ALIVE && !wide_st[sw2_find(700)].fp_ok, "news introduces an id without a key");
    /* the news of an id carries the key kept for it */
    {
        SWIM2_PKT o; SWIM2_FPS t; UB *pt = (UB *)&t; INT k, got = 0;
        for (UW i = 0; i < sizeof t; i++) pt[i] = 0;
        INT c = swim_wide_fill(&o, &t);
        for (k = 0; k < c; k++)
            if (o.gossip[k].node_id == 299 && sw_fp_cmp(t.fp[1 + k], Y) == 0) got = 1;
        /* 11-21 (CI 44838481621 plain I3 red): on the relay the id's slot follows
         * whoever spoke last, and so does the key kept here, so the news reached
         * only the key's own holder and two nodes under one id never heard each
         * other. After a change the news carries the previous key too, so the new
         * holder of the slot hears the old key. 11-23: the 1st and 3rd news (it
         * was the 2nd and 4th; see "two keys heard in turn" below) */
        I6(c == 2 && !got, "the first news of 299 after the change does not carry the kept key");
        INT gotx = 0;
        for (k = 0; k < c; k++)
            if (o.gossip[k].node_id == 299 && sw_fp_cmp(t.fp[1 + k], X) == 0) gotx = 1;
        I6(gotx, "the first news of 299 after the change carries the previous key");
        for (k = 0; k < c; k++)
            if (o.gossip[k].node_id == 700) I6(sw_fp_zero(t.fp[1 + k]), "news of 700 carries no key");
        INT goty = 0;
        for (UW i = 0; i < sizeof t; i++) pt[i] = 0;
        c = swim_wide_fill(&o, &t);
        for (k = 0; k < c; k++)
            if (o.gossip[k].node_id == 299) {
                if (sw_fp_cmp(t.fp[1 + k], X) == 0) gotx = 2;
                if (sw_fp_cmp(t.fp[1 + k], Y) == 0) goty = 1;
            }
        I6(goty && gotx != 2, "the next news of 299 carries the kept key");
        for (INT r = 0; r < 4; r++) { for (UW i = 0; i < sizeof t; i++) pt[i] = 0; c = swim_wide_fill(&o, &t); }
        got = 0;
        for (k = 0; k < c; k++)
            if (o.gossip[k].node_id == 299 && sw_fp_cmp(t.fp[1 + k], Y) == 0) got = 1;
        I6(got, "and later news the kept key again");
    }
    /* 11-23 (n2c root, #46 4): two nodes under one id both beacon every second, so
     * a third node hears them in turn and its kept key flips before nearly every
     * news it sends. The relay slot of the id is whoever spoke last, the same node
     * whose key is kept here: news carrying the kept key reaches only that key's
     * own holder. If the previous key waits for the 2nd news after a change, it
     * never goes out while the two keep talking (each change starts the count
     * again), and the larger hears the smaller late or never (§15: 17 s, then
     * both redrew). The news right after a change must carry the previous key. */
    {
        SWIM2_PKT o; SWIM2_FPS t; UB *pt = (UB *)&t; INT k, c, other = 0, own = 0;
        for (INT r = 0; r < 4; r++) {
            const UB *first = (r & 1) ? Y : X, *last = (r & 1) ? X : Y;
            n = sw3_pkt(buf, 299, 0xFFFF, first, 0);
            swim_wide_rx(ip, buf, n);
            n = sw3_pkt(buf, 299, 0xFFFF, last, 0);
            swim_wide_rx(ip, buf, n);
            for (UW i = 0; i < sizeof t; i++) pt[i] = 0;
            c = swim_wide_fill(&o, &t);
            for (k = 0; k < c; k++)
                if (o.gossip[k].node_id == 299) {
                    if (sw_fp_cmp(t.fp[1 + k], first) == 0) other++;
                    if (sw_fp_cmp(t.fp[1 + k], last) == 0) own++;
                }
        }
        I6(other == 4 && own == 0, "two keys heard in turn: every news carries the key before the last");
        /* when the talking stops (one moved away), the news returns to the kept key */
        for (INT r = 0; r < 4; r++) { for (UW i = 0; i < sizeof t; i++) pt[i] = 0; c = swim_wide_fill(&o, &t); }
        INT kept = 0;
        for (k = 0; k < c; k++)
            if (o.gossip[k].node_id == 299 && sw_fp_cmp(t.fp[1 + k], X) == 0) kept = 1;
        I6(kept, "after the 4 news the kept key again");
    }
    /* this node's own id with another key: a redraw only when mine is the larger */
    PK_NOSW_ENTER();
    swim_wide_me = 400;
    for (INT i = 0; i < 8; i++) sw_my_fp[i] = M[i];
    sw_my_fp_ok = 1;
    PK_NOSW_LEAVE();
    n = sw3_pkt(buf, 43, 400, 0, H);
    swim_wide_rx(ip, buf, n);
    I6(sw_redraw_asked == sv_ask, "a larger key on my id: I keep it");
    n = sw3_pkt(buf, 43, 400, 0, M);
    swim_wide_rx(ip, buf, n);
    I6(sw_redraw_asked == sv_ask, "my own key on my id: nothing");
    n = sw3_pkt(buf, 43, 400, 0, 0);
    swim_wide_rx(ip, buf, n);
    I6(sw_redraw_asked == sv_ask, "no key on my id: nothing");
    n = sw3_pkt(buf, 43, 400, 0, L);
    swim_wide_rx(ip, buf, n);
    I6(sw_redraw_asked == sv_ask + 1, "a smaller key on my id: I redraw");
    n = sw3_pkt(buf, 400, 0xFFFF, L, 0);
    swim_wide_rx(ip, buf, n);
    I6(sw_redraw_asked == sv_ask + 2, "my id's beacon with a smaller key (a LAN): I redraw");
    I6(sw2_find(400) < 0, "my own id stays out of the table");
    /* a count larger than the packet is cut to what is there */
    n = sw3_pkt(buf, 43, 400, 0, L);
    buf[sizeof(SWIM2_PKT) + 3] = 200;
    swim_wide_rx(ip, buf, (UH)(sizeof(SWIM2_PKT) + 4 + 8));   /* only fp[0] is there */
    I6(sw_redraw_asked == sv_ask + 2, "a key past the end is not read");
    /* the smaller side: still hearing the larger key 10 s after it first did */
    {
        UW t = 50000;
        sw_claim_seen = 0;
        swim_wide_claim(H, t);
        I6(sw_redraw_asked == sv_ask + 2 && sw_claim_seen, "a larger key: the first time I wait");
        swim_wide_claim(H, t + SWIM2_CLAIM_WAIT_MS - 1);
        I6(sw_redraw_asked == sv_ask + 2, "still within 10 s: I wait");
        /* 11-21 (audit-208): the larger may have redrawn and not yet spoken under
         * its new id; its old key still comes for a while (a third node's pin until
         * it turns SUSPECT, the 2nd and 4th news after a change). One hearing past
         * the 10 s is not enough: it must come again SWIM2_CLAIM_AGAIN_MS later. */
        swim_wide_claim(H, t + SWIM2_CLAIM_WAIT_MS);
        I6(sw_redraw_asked == sv_ask + 2, "heard at 10 s: I wait for one more");
        swim_wide_claim(H, t + SWIM2_CLAIM_WAIT_MS + SWIM2_CLAIM_AGAIN_MS - 1);
        I6(sw_redraw_asked == sv_ask + 2, "again, but within 7 s of that: I wait");
        swim_wide_claim(H, t + SWIM2_CLAIM_WAIT_MS + SWIM2_CLAIM_AGAIN_MS);
        I6(sw_redraw_asked == sv_ask + 3, "heard at 10 s and 7 s after: the larger has not moved, I redraw");
        sw_claim_seen = 0;
        swim_wide_claim(H, 0xFFFFF000UL);
        swim_wide_claim(H, (UW)(0xFFFFF000UL + SWIM2_CLAIM_WAIT_MS));
        swim_wide_claim(H, (UW)(0xFFFFF000UL + SWIM2_CLAIM_WAIT_MS + SWIM2_CLAIM_AGAIN_MS));
        I6(sw_redraw_asked == sv_ask + 4, "the 10 s and the 7 s across the clock's wrap");
        /* the shape seen in /build/mvk/l-fall/r1 (audit-208): the larger redrew at
         * once, its old key came at 10 s, 12 s, 14 s, then never */
        sw_claim_seen = 0;
        swim_wide_claim(H, 90000);
        swim_wide_claim(H, 90000 + SWIM2_CLAIM_WAIT_MS);
        swim_wide_claim(H, 90000 + SWIM2_CLAIM_WAIT_MS + 2000);
        swim_wide_claim(H, 90000 + SWIM2_CLAIM_WAIT_MS + 4000);
        I6(sw_redraw_asked == sv_ask + 4, "a moved larger key's tail at 10, 12, 14 s: I keep my id");
    }
    /* 11-21 (audit-207, CI 44880448567 plain I3 red, both redrew): a key already
     * heard under another wide id is a node that moved away from mine. Its old
     * key still comes as news of my id (a third node's pin, the 2nd and 4th news
     * carry the previous key), late; it must not make me redraw. */
    {
        static const UB L2[8] = { 0x41, 0, 0, 0, 0, 0, 0, 2 };
        static const UB L3[8] = { 0x42, 0, 0, 0, 0, 0, 0, 3 };
        static const UB L4[8] = { 0x43, 0, 0, 0, 0, 0, 0, 4 };
        static const UB H2[8] = { 0xC1, 0, 0, 0, 0, 0, 0, 2 };
        UW a = sw_redraw_asked;
        /* heard straight under 500, then as news of my id: the larger side waits for nothing */
        n = sw3_pkt(buf, 500, 0xFFFF, L, 0);
        swim_wide_rx(ip, buf, n);
        n = sw3_pkt(buf, 43, 400, 0, L);
        swim_wide_rx(ip, buf, n);
        I6(sw_redraw_asked == a, "a key heard under 500 comes as my id's news: it moved, I keep my id");
        /* the smaller side: the larger key heard under 501 never starts the 10 s */
        n = sw3_pkt(buf, 501, 0xFFFF, H2, 0);
        swim_wide_rx(ip, buf, n);
        sw_claim_seen = 0;
        swim_wide_claim(H2, 70000);
        swim_wide_claim(H2, 70000 + SWIM2_CLAIM_WAIT_MS);
        I6(sw_redraw_asked == a && !sw_claim_seen, "the larger key moved to 501: the smaller does not redraw");
        /* only in the news of another id (no beacon from it yet) */
        n = sw3_pkt(buf, 43, 502, 0, L2);
        swim_wide_rx(ip, buf, n);
        n = sw3_pkt(buf, 43, 400, 0, L2);
        swim_wide_rx(ip, buf, n);
        I6(sw_redraw_asked == a, "a key in the news of 502 comes as my id's news: it moved");
        /* in one packet, my id's news first and the other id's after it */
        n = sw3_pkt(buf, 43, 400, 0, L3);
        {
            SWIM2_PKT *pp = (SWIM2_PKT *)buf;
            SWIM2_FPS *tt = (SWIM2_FPS *)(buf + sizeof(SWIM2_PKT));
            pp->gossip_cnt = 2;
            pp->gossip[1].node_id = 503; pp->gossip[1].state = DNODE_ALIVE;
            tt->n = 3;
            for (INT i = 0; i < 8; i++) tt->fp[2][i] = L3[i];
            n = (UH)(sizeof(SWIM2_PKT) + 4 + 24);
        }
        swim_wide_rx(ip, buf, n);
        I6(sw_redraw_asked == a, "one packet: my id with L3, then 503 with L3: it moved");
        /* a key heard nowhere else still redraws (the check is not a blanket) */
        n = sw3_pkt(buf, 43, 400, 0, L4);
        swim_wide_rx(ip, buf, n);
        I6(sw_redraw_asked == a + 1, "a smaller key heard nowhere else: I redraw");
    }

    PK_NOSW_ENTER();
    wide_map = saved_map;
    for (INT i = 0; i < DREGION_MAX; i++) wide_st[i] = saved[i];
    swim_wide_me = sv_me; sw_my_fp_ok = sv_fp_ok;
    for (INT i = 0; i < 8; i++) sw_my_fp[i] = sv_fp[i];
    sw_keychg = sv_chg; sw_redraw_asked = sv_ask;
    sw_claim_seen = sv_seen; sw_claim_t0 = sv_t0; sw_claim_t1 = sv_t1;
    sw_mv_n = sv_mv_n; sw_mv_w = sv_mv_w;
    for (INT i = 0; i < SW_MOVED_N; i++) for (INT j = 0; j < 8; j++) sw_mv_fp[i][j] = sv_mv[i][j];
    PK_NOSW_LEAVE();
#undef I6
    emit(fails ? "[idpin] RED\r\n" : "[idpin] ALL PASS\r\n");
    SW_SELFTEST_LEAVE();
    return fails;
}
#endif /* _TK_HOSTED_LIBC_ */

/* ------------------------------------------------------------------ */
/* SWIM-INCARN self-test (external audit 2026-06-13, gap-ledger)       */
/*                                                                     */
/* Drives the REAL gossip_apply()/gossip_supersedes() with crafted     */
/* SWIM_PKTs and asserts the canonical anti-stale-rumor behaviour:     */
/*  (a) a STALE DEAD rumor about SELF is REFUTED — the node is NOT      */
/*      marked dead, my_incarnation is bumped strictly above the rumor, */
/*      and an ALIVE-about-self is queued at the fresh incarnation;     */
/*  (b) a peer's stale LOWER-incarnation DEAD does NOT override the     */
/*      higher-incarnation ALIVE we already hold (the dead mechanism    */
/*      is now alive), while a strictly HIGHER-incarnation rumor does.  */
/* Emits "[swim-incarn] PASS" (returns 0) or "[swim-incarn] FAIL ...". */
/* ------------------------------------------------------------------ */

/* find the incarnation a node_id is currently queued for in gq[] (or -1) */
static INT gq_find_incarn(pk_node_t node_id, UB state)
{
    for (INT i = 0; i < gq_cnt; i++)
        if (gq[i].node_id == node_id && gq[i].state == state && gq[i].remaining > 0)
            return (INT)gq[i].incarnation;
    return -1;
}

/* find the capability BYTE a node_id is currently queued for in gq[] (or -1).
 * Used by the teacher cert to inspect what an rx-rediscovery relay ENQUEUED
 * for epidemic re-propagation (the bit-1 drop the auditor caught lived here,
 * invisible to a cert that only drives gossip_apply). */
static INT gq_find_cap(pk_node_t node_id, UB state)
{
    for (INT i = 0; i < gq_cnt; i++)
        if (gq[i].node_id == node_id && gq[i].state == state && gq[i].remaining > 0)
            return (INT)gq[i].capability;
    return -1;
}

INT swim_incarn_self_test(void (*emit)(const char *))
{
    SW_SELFTEST_ENTER();
    INT fails = 0;
    void (*say)(const char *) = emit ? emit : sw_puts;

    /* --- save global/file state we will perturb, restore at the end --- */
    pk_node_t saved_my = drpc_my_node;
    UB  saved_inc     = my_incarnation;
    INT saved_gq_cnt  = gq_cnt;

    const UB SELF = 0;          /* pretend to be node 0 for the test */
    const UB PEER = 1;

    drpc_my_node            = SELF;
    gq_cnt                  = 0;            /* clear the gossip queue */
    my_incarnation          = 0;
    DROWW(SELF).state = DNODE_ALIVE;
    SWT_INC(SELF)      = 0;
    DROWW(PEER).state = DNODE_UNKNOWN;
    SWT_INC(PEER)      = 0;

    /* (a) a STALE DEAD rumor about SELF at incarnation 1 ------------- */
    {
        SWIM_PKT p = { 0 };
        p.gossip_cnt           = 1;
        p.gossip[0].node_id    = SELF;
        p.gossip[0].state      = DNODE_DEAD;
        p.gossip[0].incarnation = 1;
        gossip_apply(&p);

        if (DROW(SELF).state == DNODE_DEAD) {
            say("[swim-incarn] FAIL self-marked-dead (stale DEAD rumor killed me)\r\n");
            fails++;
        }
        if (my_incarnation <= 1) {
            say("[swim-incarn] FAIL self-incarnation-not-bumped-above-rumor\r\n");
            fails++;
        }
        INT q = gq_find_incarn(SELF, DNODE_ALIVE);
        if (q < 0) {
            say("[swim-incarn] FAIL self-ALIVE-refutation-not-queued\r\n");
            fails++;
        } else if ((UB)q != my_incarnation) {
            say("[swim-incarn] FAIL self-refutation-incarnation-mismatch\r\n");
            fails++;
        }
    }

    /* (b) peer anti-stale LWW: hold PEER ALIVE @ incarnation 5 ------- */
    DROWW(PEER).state = DNODE_ALIVE;
    SWT_INC(PEER)      = 5;
    {
        /* stale LOWER-incarnation DEAD must be IGNORED */
        SWIM_PKT p = { 0 };
        p.gossip_cnt            = 1;
        p.gossip[0].node_id     = PEER;
        p.gossip[0].state       = DNODE_DEAD;
        p.gossip[0].incarnation = 3;
        gossip_apply(&p);
        if (DROW(PEER).state != DNODE_ALIVE) {
            say("[swim-incarn] FAIL peer-killed-by-stale-rumor (LWW dead)\r\n");
            fails++;
        }
    }
    {
        /* strictly HIGHER-incarnation DEAD MUST be accepted */
        SWIM_PKT p = { 0 };
        p.gossip_cnt            = 1;
        p.gossip[0].node_id     = PEER;
        p.gossip[0].state       = DNODE_DEAD;
        p.gossip[0].incarnation = 6;
        gossip_apply(&p);
        if (DROW(PEER).state != DNODE_DEAD) {
            say("[swim-incarn] FAIL fresh-rumor-not-accepted (LWW stuck)\r\n");
            fails++;
        }
    }

    /* --- restore --- */
    drpc_my_node            = saved_my;
    my_incarnation          = saved_inc;
    gq_cnt                  = saved_gq_cnt;
    DROWW(SELF).state = DNODE_UNKNOWN;
    DROWW(PEER).state = DNODE_UNKNOWN;
    SWT_INC(SELF)      = 0;
    SWT_INC(PEER)      = 0;

    if (fails == 0) say("[swim-incarn] PASS\r\n");
    SW_SELFTEST_LEAVE();
    return fails;
}

/* ------------------------------------------------------------------ */
/* N-2b cap-gossip self-test (p2p-overlay.md "Supernodes (N-2)")       */
/*                                                                     */
/* Drives the REAL gossip_apply() with crafted SWIM_PKTs carrying the   */
/* capability bit and asserts the three contracts from the slice spec: */
/*  [cap-gossip-converge]  a fresh self-rumor capability=1 about X makes */
/*      the receiver report region_is_super_capable(X)==TRUE AND its     */
/*      region_supernode() select X; two receivers fed the SAME bits     */
/*      converge on the SAME supernode (no vote — NOCENTRAL).            */
/*  [cap-gossip-staleness] a stale LOWER-incarnation rumor that would     */
/*      flip X's capability is IGNORED (no regress); a fresh HIGHER one   */
/*      does update it — reuses the exact incarnation gate.              */
/*  [cap-gossip-falsifiable] the converge result is CONTRASTED against a  */
/*      capability=0 rumor: if apply ignored the byte (or a third party   */
/*      could originate it) both would agree and this assert would fail.  */
/* Emits "[cap-gossip] ..." lines; returns 0 on PASS else the fail count.*/
/* ------------------------------------------------------------------ */

INT swim_cap_gossip_self_test(void (*emit)(const char *))
{
    SW_SELFTEST_ENTER();
    INT fails = 0;
    void (*say)(const char *) = emit ? emit : sw_puts;

    /* save global/file state we perturb */
    pk_node_t saved_my = drpc_my_node;
    UB  saved_inc    = my_incarnation;
    INT saved_gq_cnt = gq_cnt;
    UB  saved_capX, saved_capSELF;

    const UB SELF = 0;   /* the receiving node we simulate */
    const UB X    = 3;   /* the supernode-capable peer being gossiped */
    const UB Y    = 5;   /* a second capable peer (lock-step convergence) */

    drpc_my_node            = SELF;
    gq_cnt                  = 0;
    my_incarnation          = 0;
    DROWW(SELF).state = DNODE_ALIVE;  SWT_INC(SELF) = 0;
    DROWW(X).state    = DNODE_UNKNOWN; SWT_INC(X)   = 0;
    DROWW(Y).state    = DNODE_UNKNOWN; SWT_INC(Y)   = 0;
    saved_capX    = region_is_super_capable(X)    ? 1 : 0;
    saved_capSELF = region_is_super_capable(SELF) ? 1 : 0;
    region_set_super_capable(X, FALSE);
    region_set_super_capable(Y, FALSE);
    region_set_super_capable(SELF, FALSE);  /* SELF is not capable here */

    /* [cap-gossip-converge] X emits its OWN ALIVE rumor, capability=1, inc=1 */
    {
        SWIM_PKT p = { 0 };
        p.gossip_cnt            = 1;
        p.gossip[0].node_id     = X;
        p.gossip[0].state       = DNODE_ALIVE;
        p.gossip[0].incarnation = 1;
        p.gossip[0].capability  = 1;
        gossip_apply(&p);
        if (region_is_super_capable(X) != TRUE) {
            say("[cap-gossip] FAIL converge: X not capable after fresh self-rumor\r\n");
            fails++;
        }
    }

    /* region_supernode() must now select X. region_recompute() needs a real
     * RTT≤tau for X to count X as a region member, so inject one (the live
     * fleet gets this from the directed PING/ACK probe). SELF is incapable, so
     * the lowest CAPABLE member is X. */
    rtt_observe(X, 5);   /* 5ms <= REGION_TAU_MS=50 -> X is a region member */
    {
        UB sn = region_supernode();
        if (sn != X) {
            say("[cap-gossip] FAIL converge: region_supernode() did not select X\r\n");
            fails++;
        }
    }

    /* NOCENTRAL convergence: a SECOND capable peer Y arrives the same way;
     * X (lower id) still wins by pure recomputation, no vote. Then a third
     * receiver fed the SAME two bits computes the SAME supernode. */
    {
        SWIM_PKT p = { 0 };
        p.gossip_cnt            = 1;
        p.gossip[0].node_id     = Y;
        p.gossip[0].state       = DNODE_ALIVE;
        p.gossip[0].incarnation = 1;
        p.gossip[0].capability  = 1;
        gossip_apply(&p);
        rtt_observe(Y, 5);
        UB sn_a = region_supernode();   /* this node's view */
        UB sn_b = region_supernode();   /* same inputs, same fn -> same id */
        if (!(sn_a == X && sn_b == X)) {
            say("[cap-gossip] FAIL converge: nodes disagree on supernode (expected X)\r\n");
            fails++;
        }
    }

    /* [cap-gossip-falsifiable] CONTRAST: a node W that gossips capability=0
     * about itself must NOT become capable. If apply ignored the byte (or a
     * third party originated capability), W would read capable and this fails.
     * Also proves a THIRD PARTY cannot originate: this rumor's node_id==W is
     * W's own word, and it says 0 -> stays 0. */
    {
        const UB W = 7;
        DROWW(W).state = DNODE_UNKNOWN; SWT_INC(W) = 0;
        region_set_super_capable(W, FALSE);
        SWIM_PKT p = { 0 };
        p.gossip_cnt            = 1;
        p.gossip[0].node_id     = W;
        p.gossip[0].state       = DNODE_ALIVE;
        p.gossip[0].incarnation = 1;
        p.gossip[0].capability  = 0;   /* W self-declares NON-capable */
        gossip_apply(&p);
        if (region_is_super_capable(W) != FALSE) {
            say("[cap-gossip] FAIL falsifiable: capability=0 rumor made W capable (byte ignored?)\r\n");
            fails++;
        }
        /* the contrast: X(=1) and W(=0) fed through the SAME apply path gave
         * DIFFERENT capability — so the byte is genuinely read. */
        if (region_is_super_capable(X) == region_is_super_capable(W)) {
            say("[cap-gossip] FAIL falsifiable: cap=1 and cap=0 gave same result\r\n");
            fails++;
        }
        DROWW(W).state = DNODE_UNKNOWN; SWT_INC(W) = 0;
        region_set_super_capable(W, FALSE);
    }

    /* [cap-gossip-staleness] hold X capable @ incarnation 5; a STALE
     * lower-incarnation rumor (inc=3) that would flip X to NON-capable must
     * be IGNORED — capability does not regress on a stale rumor. */
    DROWW(X).state = DNODE_ALIVE; SWT_INC(X) = 5;
    region_set_super_capable(X, TRUE);
    {
        SWIM_PKT p = { 0 };
        p.gossip_cnt            = 1;
        p.gossip[0].node_id     = X;
        p.gossip[0].state       = DNODE_ALIVE;
        p.gossip[0].incarnation = 3;   /* STALE */
        p.gossip[0].capability  = 0;   /* would flip X off if accepted */
        gossip_apply(&p);
        if (region_is_super_capable(X) != TRUE) {
            say("[cap-gossip] FAIL staleness: stale rumor flipped X's capability\r\n");
            fails++;
        }
    }
    /* a strictly HIGHER-incarnation self-rumor DOES update it (off). */
    {
        SWIM_PKT p = { 0 };
        p.gossip_cnt            = 1;
        p.gossip[0].node_id     = X;
        p.gossip[0].state       = DNODE_ALIVE;
        p.gossip[0].incarnation = 6;   /* FRESH */
        p.gossip[0].capability  = 0;
        gossip_apply(&p);
        if (region_is_super_capable(X) != FALSE) {
            say("[cap-gossip] FAIL staleness: fresh higher-incarnation rumor not applied\r\n");
            fails++;
        }
    }

    /* --- restore --- */
    drpc_my_node            = saved_my;
    my_incarnation          = saved_inc;
    gq_cnt                  = saved_gq_cnt;
    DROWW(SELF).state = DNODE_UNKNOWN;
    DROWW(X).state    = DNODE_UNKNOWN;
    DROWW(Y).state    = DNODE_UNKNOWN;
    SWT_INC(SELF) = 0; SWT_INC(X) = 0; SWT_INC(Y) = 0;
    SWT_RTTV(X) = 0; SWT_RTTV(Y) = 0;        /* drop injected RTT */
    region_set_super_capable(X, saved_capX ? TRUE : FALSE);
    region_set_super_capable(Y, FALSE);
    region_set_super_capable(SELF, saved_capSELF ? TRUE : FALSE);

    if (fails == 0) say("[cap-gossip] PASS (converge + staleness + falsifiable)\r\n");
    SW_SELFTEST_LEAVE();
    return fails;
}

/* ------------------------------------------------------------------ */
/* T-fix-a teacher-gossip self-test (thread-t-impl-plan.md §2.3)        */
/*                                                                     */
/* EXACT mirror of swim_cap_gossip_self_test for capability BIT 1       */
/* (teacher-capable). Drives the REAL gossip_apply() with crafted       */
/* SWIM_PKTs carrying capability bit 1 and asserts:                     */
/*  [teacher-converge]   a fresh self-rumor with teacher-bit=1 about X   */
/*      makes the receiver report region_is_teacher_capable(X)==TRUE AND */
/*      region_teacher() select X; a second capable peer Y arrives the   */
/*      same way and X (lower id) still wins by pure recomputation; two  */
/*      receivers fed the SAME bits converge on the SAME teacher — NO    */
/*      vote (NOCENTRAL).                                                */
/*  [teacher-selector]/[teacher-determinism]  region_teacher() = lowest- */
/*      id teacher-capable MEMBER, identical no matter who computes it.  */
/*  [teacher-staleness]  a stale LOWER-incarnation rumor that would flip  */
/*      X's teacher bit is IGNORED (no regress); a fresh HIGHER one does. */
/*  [teacher-falsifiable]  a teacher-bit=0 self-rumor must NOT make W a   */
/*      teacher; cap=1 vs cap=0 through the SAME apply give DIFFERENT     */
/*      results (proves bit 1 is genuinely read); and a rumor that sets   */
/*      ONLY bit 0 (supernode) leaves the teacher axis UNTOUCHED, while a */
/*      rumor that sets ONLY bit 1 leaves the supernode axis UNTOUCHED    */
/*      (the two capability axes do not leak into each other).           */
/* Emits "[teacher-gossip] ..." lines; returns 0 on PASS else fails.     */
/* ------------------------------------------------------------------ */

INT swim_teacher_gossip_self_test(void (*emit)(const char *))
{
    SW_SELFTEST_ENTER();
    INT fails = 0;
    void (*say)(const char *) = emit ? emit : sw_puts;

    /* save global/file state we perturb */
    pk_node_t saved_my = drpc_my_node;
    UB  saved_inc    = my_incarnation;
    INT saved_gq_cnt = gq_cnt;
    UB  saved_tchX, saved_tchSELF;

    const UB SELF = 0;   /* the receiving node we simulate */
    const UB X    = 3;   /* the teacher-capable peer being gossiped */
    const UB Y    = 5;   /* a second capable peer (lock-step convergence) */

    drpc_my_node            = SELF;
    gq_cnt                  = 0;
    my_incarnation          = 0;
    DROWW(SELF).state = DNODE_ALIVE;  SWT_INC(SELF) = 0;
    DROWW(X).state    = DNODE_UNKNOWN; SWT_INC(X)   = 0;
    DROWW(Y).state    = DNODE_UNKNOWN; SWT_INC(Y)   = 0;
    saved_tchX    = region_is_teacher_capable(X)    ? 1 : 0;
    saved_tchSELF = region_is_teacher_capable(SELF) ? 1 : 0;
    region_set_teacher_capable(X, FALSE);
    region_set_teacher_capable(Y, FALSE);
    region_set_teacher_capable(SELF, FALSE);  /* SELF is not a teacher here */
    /* clear the supernode axis on these ids too so the no-leak checks below
     * start from a known (supernode=0) baseline. */
    region_set_super_capable(X, FALSE);
    region_set_super_capable(Y, FALSE);
    region_set_super_capable(SELF, FALSE);

    /* [teacher-converge] X emits its OWN ALIVE rumor, teacher-bit=1 (capability
     * = 1<<1 = 2), inc=1. bit 0 (supernode) is left 0. */
    {
        SWIM_PKT p = { 0 };
        p.gossip_cnt            = 1;
        p.gossip[0].node_id     = X;
        p.gossip[0].state       = DNODE_ALIVE;
        p.gossip[0].incarnation = 1;
        p.gossip[0].capability  = (UB)(1u << 1);   /* teacher only */
        gossip_apply(&p);
        if (region_is_teacher_capable(X) != TRUE) {
            say("[teacher-gossip] FAIL converge: X not teacher after fresh self-rumor\r\n");
            fails++;
        }
        /* no-leak: a teacher-only rumor must NOT make X supernode-capable. */
        if (region_is_super_capable(X) != FALSE) {
            say("[teacher-gossip] FAIL no-leak: teacher-bit rumor flipped X's supernode bit\r\n");
            fails++;
        }
    }

    /* region_teacher() must now select X. region_recompute() needs a real
     * RTT≤tau for X to count X as a region member, so inject one (the live
     * fleet gets this from the directed PING/ACK probe). SELF is not a teacher,
     * so the lowest TEACHER-CAPABLE member is X. */
    rtt_observe(X, 5);   /* 5ms <= REGION_TAU_MS=50 -> X is a region member */
    {
        UB tn = region_teacher();
        if (tn != X) {
            say("[teacher-gossip] FAIL selector: region_teacher() did not select X\r\n");
            fails++;
        }
    }

    /* NOCENTRAL convergence: a SECOND capable peer Y arrives the same way;
     * X (lower id) still wins by pure recomputation, no vote. Then the same
     * inputs computed twice give the SAME teacher (determinism). */
    {
        SWIM_PKT p = { 0 };
        p.gossip_cnt            = 1;
        p.gossip[0].node_id     = Y;
        p.gossip[0].state       = DNODE_ALIVE;
        p.gossip[0].incarnation = 1;
        p.gossip[0].capability  = (UB)(1u << 1);   /* teacher only */
        gossip_apply(&p);
        rtt_observe(Y, 5);
        UB tn_a = region_teacher();   /* this node's view */
        UB tn_b = region_teacher();   /* same inputs, same fn -> same id */
        if (!(tn_a == X && tn_b == X)) {
            say("[teacher-gossip] FAIL determinism: disagree on teacher (expected X)\r\n");
            fails++;
        }
    }

    /* [teacher-staleness-death] survives teacher death by recomputation: mark
     * X DEAD (drops out of membership) → region_teacher() must hand off to the
     * next teacher-capable member Y, with NO election call. */
    {
        UB oldst = DROW(X).state;
        DROWW(X).state = DNODE_DEAD;
        UB tn = region_teacher();
        if (tn != Y) {
            say("[teacher-gossip] FAIL survives-death: X dead but teacher did not hand off to Y\r\n");
            fails++;
        }
        DROWW(X).state = oldst;   /* restore for the staleness arm */
    }

    /* [teacher-falsifiable] CONTRAST: a node W that gossips teacher-bit=0
     * about itself must NOT become a teacher. If apply ignored bit 1 (or a
     * third party originated it), W would read teacher-capable and this fails.
     * Also a SUPERNODE-only rumor (bit 0) about W must NOT make W a teacher —
     * proves the apply reads bit 1 specifically, not "any nonzero byte". */
    {
        const UB W = 7;
        DROWW(W).state = DNODE_UNKNOWN; SWT_INC(W) = 0;
        region_set_teacher_capable(W, FALSE);
        region_set_super_capable(W, FALSE);
        SWIM_PKT p = { 0 };
        p.gossip_cnt            = 1;
        p.gossip[0].node_id     = W;
        p.gossip[0].state       = DNODE_ALIVE;
        p.gossip[0].incarnation = 1;
        p.gossip[0].capability  = (UB)(1u << 0);   /* supernode ONLY, teacher=0 */
        gossip_apply(&p);
        if (region_is_teacher_capable(W) != FALSE) {
            say("[teacher-gossip] FAIL falsifiable: supernode-only rumor made W a teacher (bit1 ignored?)\r\n");
            fails++;
        }
        /* and the supernode axis DID land for W (the byte was read, just not as
         * a teacher) — confirms the apply distinguishes the two bits. */
        if (region_is_super_capable(W) != TRUE) {
            say("[teacher-gossip] FAIL no-leak: supernode-only rumor did not set W's supernode bit\r\n");
            fails++;
        }
        /* the contrast: X(teacher=1) and W(teacher=0) fed through the SAME apply
         * path gave DIFFERENT teacher-capability — so bit 1 is genuinely read. */
        if (region_is_teacher_capable(X) == region_is_teacher_capable(W)) {
            say("[teacher-gossip] FAIL falsifiable: teacher=1 and teacher=0 gave same result\r\n");
            fails++;
        }
        DROWW(W).state = DNODE_UNKNOWN; SWT_INC(W) = 0;
        region_set_teacher_capable(W, FALSE);
        region_set_super_capable(W, FALSE);
    }

    /* [teacher-staleness] hold X teacher @ incarnation 5; a STALE
     * lower-incarnation rumor (inc=3) that would flip X to NON-teacher must be
     * IGNORED — teacher-capability does not regress on a stale rumor. */
    DROWW(X).state = DNODE_ALIVE; SWT_INC(X) = 5;
    region_set_teacher_capable(X, TRUE);
    {
        SWIM_PKT p = { 0 };
        p.gossip_cnt            = 1;
        p.gossip[0].node_id     = X;
        p.gossip[0].state       = DNODE_ALIVE;
        p.gossip[0].incarnation = 3;          /* STALE */
        p.gossip[0].capability  = 0;          /* would flip X off if accepted */
        gossip_apply(&p);
        if (region_is_teacher_capable(X) != TRUE) {
            say("[teacher-gossip] FAIL staleness: stale rumor flipped X's teacher bit\r\n");
            fails++;
        }
    }
    /* a strictly HIGHER-incarnation self-rumor DOES update it (off). */
    {
        SWIM_PKT p = { 0 };
        p.gossip_cnt            = 1;
        p.gossip[0].node_id     = X;
        p.gossip[0].state       = DNODE_ALIVE;
        p.gossip[0].incarnation = 6;          /* FRESH */
        p.gossip[0].capability  = 0;
        gossip_apply(&p);
        if (region_is_teacher_capable(X) != FALSE) {
            say("[teacher-gossip] FAIL staleness: fresh higher-incarnation rumor not applied\r\n");
            fails++;
        }
    }

    /* [teacher-rx-relay] (audit fix, swim.c rx-rediscovery): the FOUR relay
     * sites in swim_task were converted to cap_byte(); the FIFTH — the
     * rediscovery re-propagation in swim_rx() — was originally left as the
     * supernode-only relay, dropping bit 1. That site is INVISIBLE to a cert
     * that only drives gossip_apply, so exercise the REAL swim_rx() here:
     * hold a teacher-capable peer Z in a non-ALIVE state, deliver a SWIM packet
     * from Z, and assert the ENQUEUED re-propagation carries bit 1 (teacher).
     * Falsifiable: if the relay drops bit 1, the queued capability is 0. */
    {
        const UB Z = 9;
        UB saved_Zst  = DROW(Z).state;
        UB saved_Zinc = SWT_INC(Z);
        DROWW(Z).state    = DNODE_UNKNOWN;   /* not ALIVE -> triggers relay */
        SWT_INC(Z)         = 2;
        region_set_teacher_capable(Z, TRUE);       /* Z is a known teacher locally */
        region_set_super_capable(Z, FALSE);        /* but NOT a supernode          */
        gq_cnt = 0;                                /* clear so the relay is isolated */

        SWIM_PKT rxp = { 0 };
        rxp.magic        = SWIM_MAGIC;
        rxp.version      = SWIM_VERSION;
        rxp.type         = SWIM_ACK;   /* seq=0 beacon-style: no probe reply */
        rxp.seq          = 0;
        rxp.src_node     = Z;
        rxp.probe_target = Z;
        rxp.gossip_cnt   = 0;          /* no piggybacked gossip; isolate the relay */
        swim_rx(swim_node_ip(Z), SWIM_PORT, (const UB *)&rxp, (UH)sizeof(rxp));

        INT qcap = gq_find_cap(Z, DNODE_ALIVE);
        if (qcap < 0) {
            say("[teacher-gossip] FAIL rx-relay: rediscovery did not enqueue Z\r\n");
            fails++;
        } else if (((UB)qcap & (UB)(1u << 1)) == 0) {
            say("[teacher-gossip] FAIL rx-relay: rediscovery relay dropped teacher bit 1\r\n");
            fails++;
        }
        DROWW(Z).state = saved_Zst; SWT_INC(Z) = saved_Zinc;
        region_set_teacher_capable(Z, FALSE);
        region_set_super_capable(Z, FALSE);
    }

    /* --- restore --- */
    drpc_my_node            = saved_my;
    my_incarnation          = saved_inc;
    gq_cnt                  = saved_gq_cnt;
    DROWW(SELF).state = DNODE_UNKNOWN;
    DROWW(X).state    = DNODE_UNKNOWN;
    DROWW(Y).state    = DNODE_UNKNOWN;
    SWT_INC(SELF) = 0; SWT_INC(X) = 0; SWT_INC(Y) = 0;
    SWT_RTTV(X) = 0; SWT_RTTV(Y) = 0;        /* drop injected RTT */
    region_set_teacher_capable(X, saved_tchX ? TRUE : FALSE);
    region_set_teacher_capable(Y, FALSE);
    region_set_teacher_capable(SELF, saved_tchSELF ? TRUE : FALSE);
    region_set_super_capable(X, FALSE);
    region_set_super_capable(Y, FALSE);
    region_set_super_capable(SELF, FALSE);

    if (fails == 0)
        say("[teacher-gossip] PASS (converge + selector + determinism + staleness + falsifiable + rx-relay)\r\n");
    SW_SELFTEST_LEAVE();
    return fails;
}

/* ------------------------------------------------------------------ */
/* wave-selfelect SELF-RECOGNITION self-test                           */
/*                                                                     */
/* THE BUG (latent, masked by the in-proc T-fix-b cert): region_teacher()*/
/* reads the teacher_capable[] TABLE, but a node's OWN table entry is    */
/* set ONLY in the gossip-APPLY path for RECEIVED peer gossip. The       */
/* self-beacon origination sites only ENQUEUE the self capability byte   */
/* for broadcast — they never apply it to the node's own table. So a     */
/* teacher node T makes its PEERS elect T (they apply T's received       */
/* gossip) but T's OWN region_teacher() never returns T → the cradle     */
/* teacher-gate (region_teacher()==self) never fires on the real teacher.*/
/*                                                                       */
/* THE CERT exercises the REAL self-origination path: it forces this     */
/* node teacher-capable (selfelect_force_teacher, the cert analogue of   */
/* a loaded teacher GGUF), runs SELF_APPLY_OWN_CAPABILITY() — the exact  */
/* statement the production self-beacon sites now run right after        */
/* gossip_add(self,...) — and asserts the node SELF-ELECTS:              */
/*   [selfelect] region_teacher() == drpc_my_node.                       */
/* FALSIFIER (load-bearing): building with -DSELFELECT_SKIP nulls        */
/* SELF_APPLY_OWN_CAPABILITY() to do{}while(0) → the self-apply is gone, */
/* the local teacher_capable[self] stays FALSE, and region_teacher() !=  */
/* self → the cert FAILS, reproducing the exact production bug.          */
/* Emits "[selfelect] ..." lines; 0 on PASS else fails. Hosted-only.     */
/* ------------------------------------------------------------------ */
#if defined(_TK_HOSTED_LIBC_)
INT swim_selfelect_self_test(void (*emit)(const char *))
{
    SW_SELFTEST_ENTER();
    INT fails = 0;
    void (*say)(const char *) = emit ? emit : sw_puts;

    /* save every global/file state we perturb (mirror the teacher cert) */
    pk_node_t saved_my = drpc_my_node;
    UB  saved_inc      = my_incarnation;
    INT saved_gq_cnt   = gq_cnt;
    int saved_force    = selfelect_force_teacher;

    const UB SELF = 2;   /* the node that must self-recognize as teacher */

    UB saved_st        = DROW(SELF).state;
    UB saved_inc_self  = SWT_INC(SELF);
    UB saved_tch       = region_is_teacher_capable(SELF) ? 1 : 0;
    UB saved_sup       = region_is_super_capable(SELF)   ? 1 : 0;
    UB saved_rttv      = SWT_RTTV(SELF);

    drpc_my_node            = SELF;
    gq_cnt                  = 0;
    my_incarnation          = 0;
    DROWW(SELF).state = DNODE_ALIVE;  SWT_INC(SELF) = 0;
    /* SELF must be a region MEMBER for region_teacher() to consider it:
     * recompute counts a member by RTT<=tau; a node's own RTT is 0 in the
     * live fleet, but inject one explicitly so the cert is self-contained. */
    rtt_observe(SELF, 0);
    /* start from the BUG baseline: SELF's own teacher bit is NOT set (this is
     * exactly the state at boot before any self-apply). */
    region_set_teacher_capable(SELF, FALSE);
    region_set_super_capable(SELF, FALSE);

    /* PRE: with teacher_self() forced true but NO self-apply yet, the node does
     * NOT yet self-elect (the bug's starting condition — proves the apply is
     * what changes the result, not the forced flag alone). */
    selfelect_force_teacher = 1;
    {
        UB pre = region_teacher();
        if (pre == SELF) {
            say("[selfelect] FAIL pre: self-elected BEFORE self-apply (table pre-seeded?)\r\n");
            fails++;
        }
    }

    /* THE PRODUCTION STATEMENT: originate self-beacon (enqueue) then self-apply
     * — the exact pair the live self-beacon sites now run. */
    gossip_add(drpc_my_node, DNODE_ALIVE, my_incarnation, cap_self_byte());
    SELF_APPLY_OWN_CAPABILITY();

    /* POST: the node now SELF-ELECTS — region_teacher() == self. With the fix
     * present this PASSES; with -DSELFELECT_SKIP the self-apply is a no-op and
     * region_teacher() != self → this FAILS (the load-bearing falsifier). */
    {
        UB post = region_teacher();
        if (post != SELF) {
            say("[selfelect] FAIL: region_teacher() != self after self-beacon "
                "(node does not self-recognize as the elected teacher)\r\n");
            fails++;
        }
        /* and the local table bit is the mechanism: it must now read TRUE. */
        if (region_is_teacher_capable(SELF) != TRUE) {
            say("[selfelect] FAIL: own teacher_capable[self] not set by self-apply\r\n");
            fails++;
        }
    }

    /* CONTRAST: a non-teacher self (force off) self-applies teacher=FALSE → it
     * must NOT self-elect (single-teacher invariant: only a teacher-capable
     * node crowns itself). */
    selfelect_force_teacher = 0;
    SELF_APPLY_OWN_CAPABILITY();
    {
        UB tn = region_teacher();
        if (tn == SELF) {
            say("[selfelect] FAIL contrast: non-teacher self-elected as teacher\r\n");
            fails++;
        }
    }

    /* --- restore --- */
    selfelect_force_teacher = saved_force;
    drpc_my_node            = saved_my;
    my_incarnation          = saved_inc;
    gq_cnt                  = saved_gq_cnt;
    DROWW(SELF).state = saved_st;
    SWT_INC(SELF)      = saved_inc_self;
    SWT_RTTV(SELF)         = saved_rttv;
    region_set_teacher_capable(SELF, saved_tch ? TRUE : FALSE);
    region_set_super_capable(SELF, saved_sup ? TRUE : FALSE);

    if (fails == 0)
        say("[selfelect] PASS (node self-recognizes its own teacher role: region_teacher()==self)\r\n");
    SW_SELFTEST_LEAVE();
    return fails;
}

/* ------------------------------------------------------------------ */
/* N2 (ii)(a) `nodes v2cap`: capability bit 2 = "this node reads v2"    */
/* (research/n2-wire16.md 2.2). Drives the REAL gossip_apply / cap_byte */
/* with crafted SWIM_PKTs, as the teacher cert does for bit 1:          */
/*   [v2cap-self]     a hosted node's own byte carries bit 2            */
/*   [v2cap-converge] a fresh self-rumor with bit 2 -> reads v2         */
/*   [v2cap-absent]   a rumor with bits 0,1 only (an old node) -> not,  */
/*                    while bits 0 and 1 still land (the byte is read)  */
/*   [v2cap-noleak]   bit 2 alone sets neither supernode nor teacher    */
/*   [v2cap-stale]    a stale rumor without bit 2 does not clear it     */
/*   [v2cap-relay]    the re-propagated entry keeps bit 2               */
/*   [v2cap-wide]     a wide id heard over v2 reads v2; unknown does not */
/* Falsifiers: -DN2_NC_NOBIT2 (no bit 2 sent), -DN2_NC_V2IGNORE (bit 2  */
/* not read), -DN2_NC_V2WIDE_NO (wide peers not v2). Hosted-only.      */
/* ------------------------------------------------------------------ */
INT swim_v2cap_self_test(void (*emit)(const char *))
{
    SW_SELFTEST_ENTER();
    INT fails = 0;
    void (*say)(const char *) = emit ? emit : sw_puts;
    pk_node_t saved_my = drpc_my_node;
    UB  saved_inc      = my_incarnation;
    INT saved_gq_cnt   = gq_cnt;
    const UB SELF = 0, X = 9, W = 10, V = 12, Y = 13, OLD = 11;
    const UB ids[4] = { X, W, V, Y };
    UB sst[4], sinc[4], ssup[4], stch[4], sv2[4];
    /* audit-147: the queue entries the rumors below enqueue would otherwise be
     * left in gq[] of a live node (gq_cnt alone was put back) and gossiped */
    static GOSSIP_QUEUED saved_gq[GOSSIP_Q_MAX];
    for (INT i = 0; i < GOSSIP_Q_MAX; i++) saved_gq[i] = gq[i];
    for (INT k = 0; k < 4; k++) {
        sst[k] = DROW(ids[k]).state; sinc[k] = SWT_INC(ids[k]);
        ssup[k] = region_is_super_capable(ids[k]) ? 1 : 0;
        stch[k] = region_is_teacher_capable(ids[k]) ? 1 : 0;
        sv2[k]  = v2cap_get(ids[k]);
    }

    drpc_my_node = SELF; my_incarnation = 0; gq_cnt = 0;
    for (INT k = 0; k < 4; k++) {
        DROWW(ids[k]).state = DNODE_UNKNOWN; SWT_INC(ids[k]) = 0;
        region_set_super_capable(ids[k], FALSE);
        region_set_teacher_capable(ids[k], FALSE);
        v2cap_set(ids[k], 0);
    }
#define V2C_FAIL(m) do { say("[v2cap] FAIL " m "\r\n"); fails++; } while (0)
#define V2C_RUMOR(id, inc, cap) do { SWIM_PKT p = { 0 }; p.gossip_cnt = 1;         \
        p.gossip[0].node_id = (id); p.gossip[0].state = DNODE_ALIVE;               \
        p.gossip[0].incarnation = (inc); p.gossip[0].capability = (UB)(cap);       \
        gossip_apply(&p); } while (0)

    if ((cap_self_byte() & (UB)(1u << 2)) == 0) V2C_FAIL("self: own capability byte lacks bit 2");

    V2C_RUMOR(X, 1, 1u << 2);
    if (!swim_peer_reads_v2(X)) V2C_FAIL("converge: fresh self-rumor with bit 2, X does not read v2");
    if (region_is_super_capable(X) || region_is_teacher_capable(X))
        V2C_FAIL("noleak: bit 2 alone set the supernode or teacher bit");

    V2C_RUMOR(W, 1, (1u << 0) | (1u << 1));
    if (swim_peer_reads_v2(W)) V2C_FAIL("absent: a rumor without bit 2 made W read v2");
    if (!region_is_super_capable(W) || !region_is_teacher_capable(W))
        V2C_FAIL("absent: bits 0 and 1 of W's rumor did not land (byte not read)");

    V2C_RUMOR(X, 0, 0);   /* stale: lower incarnation, no bit 2 */
    if (!swim_peer_reads_v2(X)) V2C_FAIL("stale: a stale rumor without bit 2 cleared X");

    {
        INT q = gq_find_cap(X, DNODE_ALIVE);
        if (q < 0 || ((UB)q & (UB)(1u << 2)) == 0) V2C_FAIL("relay: the queued entry for X lost bit 2");
    }
    V2C_RUMOR(V, 1, 0);   /* plain new peer, nothing: must not read v2 */
    if (swim_peer_reads_v2(V)) V2C_FAIL("absent: a zero byte made V read v2");

    {   /* [v2cap-oldrelay] (audit-147): an OLD node relays Y's rumor first and
         * drops bit 2 (old cap_byte has bits 0 and 1 only). Y's own word at the
         * same incarnation loses the LWW gate, and incarnations rise only on a
         * refutation, so without a direct path Y would read "not v2" for good. */
        SWIM_PKT p = { 0 }; p.src_node = OLD; p.gossip_cnt = 1;
        p.gossip[0].node_id = Y; p.gossip[0].state = DNODE_ALIVE;
        p.gossip[0].incarnation = 0; p.gossip[0].capability = 0;
        gossip_apply(&p);
        if (swim_peer_reads_v2(Y)) V2C_FAIL("oldrelay: an old relay without bit 2 made Y read v2");
        p.src_node = Y; p.gossip[0].capability = (UB)(1u << 2);
        gossip_apply(&p);
        if (!swim_peer_reads_v2(Y)) V2C_FAIL("oldrelay: Y's own same-incarnation word with bit 2 did not land");
        p.src_node = OLD;                 /* a third party may not set it */
        p.gossip[0].node_id = V; p.gossip[0].incarnation = 1;   /* V is ALIVE at 1 */
        gossip_apply(&p);
        if (swim_peer_reads_v2(V)) V2C_FAIL("oldrelay: a third party's same-incarnation bit 2 made V read v2");
        p.src_node = Y; p.gossip[0].node_id = Y; p.gossip[0].incarnation = 0;
        p.gossip[0].capability = 0;
        gossip_apply(&p);                 /* Y itself now says no (restarted old) */
        if (swim_peer_reads_v2(Y)) V2C_FAIL("oldrelay: Y's own same-incarnation word without bit 2 did not clear it");
        /* audit-148 [v2cap-lateown]: Y at inc 2 (a fresh relayed word with bit 2),
         * then Y's own LATE word at inc 1 without bit 2: only the incarnation we
         * hold may change bit 2 on the direct path */
        p.src_node = OLD; p.gossip[0].incarnation = 2; p.gossip[0].capability = (UB)(1u << 2);
        gossip_apply(&p);
        if (!swim_peer_reads_v2(Y)) V2C_FAIL("lateown: a fresh relayed word with bit 2 did not set Y");
        p.src_node = Y; p.gossip[0].incarnation = 1; p.gossip[0].capability = 0;
        gossip_apply(&p);
        if (!swim_peer_reads_v2(Y)) V2C_FAIL("lateown: Y's own older-incarnation word cleared bit 2");
    }

    {   /* on a fresh wide table, saved and put back as swim_wide_self_test does
         * (run alone, before swim_init, the table has no room at all) */
        static SWIM_WIDE saved[DREGION_MAX];
        static NODEMAP   saved_map;
        const pk_node_t WID = 777, UNK = 778;
        PK_NOSW_ENTER();
        saved_map = wide_map;
        for (INT i = 0; i < DREGION_MAX; i++) saved[i] = wide_st[i];
        nodemap_init(&wide_map, 0);
        for (INT i = 0; i < DREGION_MAX; i++) wide_st[i].state = DNODE_UNKNOWN;
        PK_NOSW_LEAVE();
        swim_wide_heard(WID, pk_node_ip(WID), TRUE, swim_wide_now(), (const UB *)0);
        if (!swim_peer_reads_v2(WID)) V2C_FAIL("wide: a wide id heard over v2 does not read v2");
        if (swim_peer_reads_v2(UNK))  V2C_FAIL("wide: an unknown wide id reads v2");
        {
            INT s = nodemap_find(&wide_map, WID);
            if (s != NODEMAP_NOSLOT) wide_st[s].state = DNODE_DEAD;
            if (swim_peer_reads_v2(WID)) V2C_FAIL("wide: a dead wide id still reads v2");
        }
        PK_NOSW_ENTER();
        wide_map = saved_map;
        for (INT i = 0; i < DREGION_MAX; i++) wide_st[i] = saved[i];
        PK_NOSW_LEAVE();
    }
#undef V2C_RUMOR
#undef V2C_FAIL

    for (INT k = 0; k < 4; k++) {
        DROWW(ids[k]).state = sst[k]; SWT_INC(ids[k]) = sinc[k];
        region_set_super_capable(ids[k], ssup[k] ? TRUE : FALSE);
        region_set_teacher_capable(ids[k], stch[k] ? TRUE : FALSE);
        v2cap_set(ids[k], sv2[k]);
    }
    for (INT i = 0; i < GOSSIP_Q_MAX; i++) gq[i] = saved_gq[i];
    drpc_my_node = saved_my; my_incarnation = saved_inc; gq_cnt = saved_gq_cnt;
    if (fails == 0) say("[v2cap] PASS (bit 2 sent, read under the LWW gate, relayed; wide ids read v2)\r\n");
    else            say("[v2cap] RED\r\n");
    SW_SELFTEST_LEAVE();
    return fails;
}
#endif /* _TK_HOSTED_LIBC_ */

/* ------------------------------------------------------------------ */
/* compat [no-fleet-split] self-test                                   */
/* (compat-migration-chain-plan.md §5.3/§6; compatibility.md §7)        */
/*                                                                     */
/* THE STRATEGY: "no frozen core; NO FLEET SPLIT". Whether two          */
/* different-version nodes stay ONE fleet is decided at swim_rx():418   */
/*   if (pkt->magic != SWIM_MAGIC || pkt->version != SWIM_VERSION)      */
/*       return;                                                        */
/* plus what gossip_apply() does with the ADDITIVE SWIM_GOSSIP_EVT      */
/* `capability` byte (swim.h:81 — the `_pad`->capability rule added     */
/* WITHOUT bumping SWIM_VERSION: the canonical "vN+1 additive field a   */
/* vN reads as zero").                                                  */
/*                                                                     */
/* This cert drives the REAL swim_rx() (NOT just gossip_apply — the     */
/* version drop happens INSIDE swim_rx at :418, ABOVE gossip_apply, so  */
/* a cert that only calls gossip_apply is BLIND to it). FOUR legs:      */
/*  [split-membership-crosses] (CURE) a version==SWIM_VERSION packet    */
/*      with capability!=0 marks peer P ALIVE (membership crosses an    */
/*      additive-change gap).                                           */
/*  [split-partition-on-bump] (FALSIFIER, load-bearing) the SAME packet */
/*      with version=SWIM_VERSION+1 does NOT mark P ALIVE — the ONLY    */
/*      difference is the single version byte, so deleting the          */
/*      `pkt->version != SWIM_VERSION` clause at swim.c:418 is the      */
/*      UNIQUE thing that flips this PASS->FAIL. P is reset to non-ALIVE */
/*      IMMEDIATELY before, so a stale ALIVE cannot make it vacuous.    */
/*  [split-additive-crosses] the capability!=0 from a valid self-       */
/*      authoritative rumor actually propagated — region_is_super_      */
/*      capable(P)==TRUE (the additive field is read, not dropped).     */
/*  [split-degrade-not-drop] a version==SWIM_VERSION, capability==0     */
/*      packet (an OLD emitter) STILL marks P ALIVE — an old node is    */
/*      NOT dropped for sending zero (the "生存情報だけは必ず通す"     */
/*      lifeline; capability=0 degrades, never partitions).             */
/*                                                                     */
/* HONEST SCOPE: this in-proc gate proves the SWIM membership/gossip    */
/* mechanism crosses a version gap for an ADDITIVE change and           */
/* partitions on a breaking BUMP. It does NOT retire compatibility.md   */
/* §7 (replica.c:282 etc. still hard-drop on version — reject->degrade  */
/* is a separate slice); it does NOT prove a real 2-OS-process          */
/* teach->answer (a DEFERRED [live] row needing a pinned-vN binary +    */
/* real host); downgrade-attack authentication is out of scope.         */
/* Emits "[no-fleet-split] ..." lines; returns 0 on PASS else fails.    */
/* ------------------------------------------------------------------ */
#if defined(NOFLEETSPLIT_CERT) && defined(_TK_HOSTED_LIBC_)
INT swim_nofleetsplit_self_test(void (*emit)(const char *))
{
    SW_SELFTEST_ENTER();
    INT fails = 0;
    void (*say)(const char *) = emit ? emit : sw_puts;

    /* --- save global/file state we perturb, restore at the end --- */
    pk_node_t saved_my = drpc_my_node;
    UB  saved_inc    = my_incarnation;
    INT saved_gq_cnt = gq_cnt;

    const UB SELF = 0;          /* the receiving node we simulate */
    const UB P    = 4;          /* the differently-versioned peer */
    UB  saved_Pst  = DROW(P).state;
    UB  saved_Pinc = SWT_INC(P);
    UW  saved_Pip  = DROW(P).ip;
    UB  saved_capP = region_is_super_capable(P) ? 1 : 0;

    drpc_my_node            = SELF;
    gq_cnt                  = 0;
    my_incarnation          = 0;
    DROWW(SELF).state = DNODE_ALIVE; SWT_INC(SELF) = 0;
    region_set_super_capable(P, FALSE);

    /* The one packet (built once); legs flip only the fields named. P emits
     * its OWN ALIVE rumor about itself with capability bit0=1 (self-
     * authoritative), piggybacked so the additive field actually crosses. */
    SWIM_PKT base = { 0 };
    base.magic            = SWIM_MAGIC;
    base.version          = SWIM_VERSION;
    base.type             = SWIM_ACK;     /* seq=0 beacon-style, no probe reply */
    base.seq              = 0;
    base.src_node         = P;
    base.probe_target     = P;
    base.gossip_cnt       = 1;
    base.gossip[0].node_id     = P;
    base.gossip[0].state       = DNODE_ALIVE;
    base.gossip[0].incarnation = 1;
    base.gossip[0].capability  = 1;       /* bit0 = supernode-capable */

    /* [split-membership-crosses] (CURE): same-version packet -> P ALIVE. */
    DROWW(P).state = DNODE_UNKNOWN; SWT_INC(P) = 0;
    {
        SWIM_PKT p = base;
        swim_rx(swim_node_ip(P), SWIM_PORT, (const UB *)&p, (UH)sizeof(p));
        if (DROW(P).state != DNODE_ALIVE) {
            say("[no-fleet-split] FAIL split-membership-crosses: same-version peer not ALIVE\r\n");
            fails++;
        } else {
            say("[no-fleet-split] split-membership-crosses ok\r\n");
        }
    }

    /* [split-additive-crosses]: the capability byte actually propagated. */
    {
        if (region_is_super_capable(P) != TRUE) {
            say("[no-fleet-split] FAIL split-additive-crosses: capability byte dropped\r\n");
            fails++;
        } else {
            say("[no-fleet-split] split-additive-crosses ok\r\n");
        }
    }

    /* [split-partition-on-bump] (FALSIFIER, load-bearing): reset P to
     * non-ALIVE, then deliver the SAME packet differing ONLY in version byte
     * (SWIM_VERSION+1) -> swim_rx():418 must DROP it; P must NOT be ALIVE.
     * The reset immediately-before is mandatory: without it a stale ALIVE
     * from the prior leg makes this assert vacuously pass. */
    DROWW(P).state = DNODE_UNKNOWN; SWT_INC(P) = 0;
    region_set_super_capable(P, FALSE);
    {
        SWIM_PKT p = base;
        p.version = (UB)(SWIM_VERSION + 1);   /* THE ONLY DIFFERENCE from leg 1 */
        swim_rx(swim_node_ip(P), SWIM_PORT, (const UB *)&p, (UH)sizeof(p));
        if (DROW(P).state == DNODE_ALIVE) {
            say("[no-fleet-split] FAIL split-partition-on-bump: breaking-version peer wrongly ALIVE\r\n");
            fails++;
        } else {
            say("[no-fleet-split] split-partition-on-bump ok\r\n");
        }
    }

    /* [split-degrade-not-drop]: an OLD emitter (same version, capability==0)
     * is STILL marked ALIVE — zero degrades, never partitions. */
    DROWW(P).state = DNODE_UNKNOWN; SWT_INC(P) = 0;
    region_set_super_capable(P, FALSE);
    {
        SWIM_PKT p = base;
        p.gossip[0].incarnation = 2;          /* fresh so the rumor is accepted */
        p.gossip[0].capability  = 0;          /* OLD node sends zero */
        swim_rx(swim_node_ip(P), SWIM_PORT, (const UB *)&p, (UH)sizeof(p));
        if (DROW(P).state != DNODE_ALIVE) {
            say("[no-fleet-split] FAIL split-degrade-not-drop: old (cap=0) peer dropped from fleet\r\n");
            fails++;
        } else {
            say("[no-fleet-split] split-degrade-not-drop ok\r\n");
        }
    }

    /* --- restore --- */
    drpc_my_node            = saved_my;
    my_incarnation          = saved_inc;
    gq_cnt                  = saved_gq_cnt;
    DROWW(SELF).state = DNODE_UNKNOWN; SWT_INC(SELF) = 0;
    DROWW(P).state    = saved_Pst;
    SWT_INC(P)         = saved_Pinc;
    DROWW(P).ip       = saved_Pip;
    region_set_super_capable(P, saved_capP ? TRUE : FALSE);

    if (fails == 0)
        say("[no-fleet-split] PASS\r\n");
    else {
        say("[no-fleet-split] FAIL ");
        sw_putdec((UW)fails);
        say("\r\n");
    }
    SW_SELFTEST_LEAVE();
    return fails;
}
#endif /* NOFLEETSPLIT_CERT && _TK_HOSTED_LIBC_ */

/* ------------------------------------------------------------------ */
/* [swim-selfsuspect] SELF-SUSPICION DE-STORM self-test (HOSTED-ONLY)   */
/*                                                                     */
/* Drives the REAL gossip-ingest (gossip_apply) with N rapid self-      */
/* SUSPECT rumors about drpc_my_node inside ONE throttle interval and   */
/* asserts:                                                            */
/*   [selfsuspect-refute]   the refute fired N times — my_incarnation   */
/*        advanced by exactly N (ALIVE re-asserted each rumor).         */
/*        Liveness/refutation is PRESERVED by the throttle.             */
/*   [selfsuspect-throttle] with the CURE build the actual scatter fired */
/*        EXACTLY once for the whole burst (self_scatter_fires == 1).    */
/*                                                                     */
/* TEETH: rebuilt with -DSWIM_NO_SCATTER_THROTTLE the throttle is gone,  */
/* self_scatter_fires == N, and [selfsuspect-throttle] goes RED. The     */
/* run_swim_selfsuspect.sh cert FAILS if the falsifier does NOT go red.  */
/*                                                                     */
/* HOSTED-ONLY: gated by SWIM_SELFSUSPECT_CERT && _TK_HOSTED_LIBC_ so    */
/* the default kernel + bare-metal crown stay byte-identical.           */
/* Emits "[selfsuspect-*] ..." + a final "[swim-selfsuspect] PASS/FAIL". */
/* Returns 0 on PASS else the fail count. */
/* ------------------------------------------------------------------ */
#if defined(SWIM_SELFSUSPECT_CERT) && defined(_TK_HOSTED_LIBC_)
INT swim_selfsuspect_self_test(void (*emit)(const char *))
{
    SW_SELFTEST_ENTER();
    INT fails = 0;
    void (*say)(const char *) = emit ? emit : sw_puts;

    const UB  SELF = 0;
    const INT N    = 8;   /* burst size (>= 8 rapid self-SUSPECT rumors) */

    /* --- save the global/file state we perturb, restore at the end --- */
    pk_node_t saved_my = drpc_my_node;
    UB  saved_inc  = my_incarnation;
    INT saved_gq   = gq_cnt;
    UB  saved_st   = DROW(SELF).state;
    UB  saved_sinc = SWT_INC(SELF);
    UW  saved_last = self_scatter_last_ms;
    INT saved_prim = self_scatter_primed;
    UW  saved_fire = self_scatter_fires;

    drpc_my_node            = SELF;
    my_incarnation          = 0;
    gq_cnt                  = 0;
    DROWW(SELF).state = DNODE_ALIVE;
    SWT_INC(SELF)      = 0;

    /* clean-slate the throttle bookkeeping so the burst is measured fresh */
    self_scatter_last_ms = 0;
    self_scatter_primed  = 0;
    self_scatter_fires   = 0;

    UB inc_before = my_incarnation;

    /* Deliver N rapid self-SUSPECT rumors. A tight loop runs FAR under
     * SELF_SCATTER_MIN_MS, so all N land within one throttle interval. The
     * self-suspect branch refutes on EVERY rumor regardless of incarnation,
     * so my_incarnation advances by exactly 1 per rumor (== N total). */
    for (INT i = 0; i < N; i++) {
        SWIM_PKT p = { 0 };
        p.magic            = SWIM_MAGIC;
        p.version          = SWIM_VERSION;
        p.type             = SWIM_ACK;   /* seq=0 beacon-style */
        p.seq              = 0;
        p.src_node         = SELF;
        p.probe_target     = SELF;
        p.gossip_cnt       = 1;
        p.gossip[0].node_id     = SELF;
        p.gossip[0].state       = DNODE_SUSPECT;
        p.gossip[0].incarnation = 0;      /* stale-or-equal: still refuted */
        p.gossip[0].capability  = 0;
        gossip_apply(&p);
    }

    UB inc_after = my_incarnation;
    UB advanced  = (UB)(inc_after - inc_before);

    /* [selfsuspect-refute]: the refute fired N times (liveness preserved). */
    if (advanced == (UB)N) {
        say("[selfsuspect-refute] PASS incarnation advanced ");
        sw_putdec((UW)advanced); say(" (== N refutes, liveness preserved)\r\n");
    } else {
        say("[selfsuspect-refute] FAIL incarnation advanced ");
        sw_putdec((UW)advanced); say(" expected "); sw_putdec((UW)N);
        say("\r\n");
        fails++;
    }

    /* [selfsuspect-throttle]: CURE -> exactly 1 scatter for the burst;
     * FALSIFIER (-DSWIM_NO_SCATTER_THROTTLE) -> N scatters -> RED. */
    say("[selfsuspect-throttle] burst=");   sw_putdec((UW)N);
    say(" scatters=");                      sw_putdec(self_scatter_fires);
    if (self_scatter_fires == 1) {
        say(" PASS (exactly 1 scatter for the whole burst)\r\n");
    } else {
        say(" FAIL (expected exactly 1 — storm not throttled)\r\n");
        fails++;
    }

    /* --- restore --- */
    drpc_my_node            = saved_my;
    my_incarnation          = saved_inc;
    gq_cnt                  = saved_gq;
    DROWW(SELF).state = saved_st;
    SWT_INC(SELF)      = saved_sinc;
    self_scatter_last_ms    = saved_last;
    self_scatter_primed     = saved_prim;
    self_scatter_fires      = saved_fire;

    if (fails == 0) {
        say("[swim-selfsuspect] PASS\r\n");
    } else {
        say("[swim-selfsuspect] FAIL ");
        sw_putdec((UW)fails);
        say("\r\n");
    }
    SW_SELFTEST_LEAVE();
    return fails;
}
#endif /* SWIM_SELFSUSPECT_CERT && _TK_HOSTED_LIBC_ */

#if defined(_TK_HOSTED_LIBC_)
/* ------------------------------------------------------------------ */
/* N2c stage 2 — `nodes life` (I7 of research/n2c-identity.md §10).   */
/* A DEAD peer comes back only with a newer incarnation of its own.  */
/* Drives the real swim_rx / gossip_apply / swim_probe_ok / drpc_rx  */
/* with crafted packets. Fixed before the code (2026-11-20).         */
/*  a1 DEAD@5 + a packet from it with no word about itself: DEAD,    */
/*     and DEAD@5 is queued (it is told, so it can refute)           */
/*  a2 DEAD@5 + its own ALIVE@5: DEAD, DEAD@5 queued                 */
/*  b  DEAD@5 + its own ALIVE@6: ALIVE at 6                          */
/*  c  DEAD@5 + a probe answer (swim_probe_ok) and a drpc HEARTBEAT: */
/*     DEAD                                                          */
/*  d  DEAD@255 + rumour ALIVE@0 (relayed): ALIVE (serial order)     */
/*  e  ALIVE@0 + rumour DEAD@255: ignored                            */
/*  f  my refute of DEAD@255 is 0, and ALIVE@0 beats DEAD@255        */
/*  g  SUSPECT@5 + a packet from it: ALIVE (as before)               */
/* Prints "[swim-life] ok|RED <item>" and PASS / FAIL.               */
/* ------------------------------------------------------------------ */
static void life_pkt(SWIM_PKT *p, UB src, UB n, UB g0, UB st0, UB inc0)
{
    UB *b = (UB *)p;
    for (INT i = 0; i < (INT)sizeof *p; i++) b[i] = 0;
    p->magic = SWIM_MAGIC; p->version = SWIM_VERSION; p->type = SWIM_ACK; p->seq = 0;
    p->src_node = src; p->probe_target = src; p->gossip_cnt = n;
    if (n) { p->gossip[0].node_id = g0; p->gossip[0].state = st0; p->gossip[0].incarnation = inc0; }
}

INT swim_life_self_test(void (*emit)(const char *))
{
    SW_SELFTEST_ENTER();
    void (*say)(const char *) = emit ? emit : sw_puts;
    INT fails = 0;
    const UB PEER = 5, THIRD = 7;
    pk_node_t saved_my = drpc_my_node;
    UB saved_inc = my_incarnation;
    INT saved_gq = gq_cnt;
    UW saved_ip = DROW(PEER).ip;
    UB saved_st = DROW(PEER).state, saved_pinc = SWT_INC(PEER);
    SWIM_PKT p;
    DRPC_PKT hb;
#define LIFE(cond, what) do { if (cond) { say("[swim-life] ok   "); } else { fails++; say("[swim-life] RED  "); } \
                              say(what); say("\r\n"); } while (0)
#define LIFE_SET(st, inc) do { DROWW(PEER).state = (st); SWT_INC(PEER) = (inc); \
                               SWT_SUS(PEER) = 0; gq_cnt = 0; } while (0)
    if (drpc_my_node == PEER || drpc_my_node == THIRD) drpc_my_node = 1;

    LIFE_SET(DNODE_DEAD, 5);
    life_pkt(&p, PEER, 0, 0, 0, 0); swim_rx(0x0A010006UL, SWIM_PORT, (const UB *)&p, sizeof p);
    LIFE(DROW(PEER).state == DNODE_DEAD, "a1s DEAD@5 + a packet with no word about itself: stays DEAD");
    LIFE(gq_find_incarn(PEER, DNODE_DEAD) == 5, "a1q DEAD@5 is queued (told)");

    LIFE_SET(DNODE_DEAD, 5);
    life_pkt(&p, PEER, 1, PEER, DNODE_ALIVE, 5); swim_rx(0x0A010006UL, SWIM_PORT, (const UB *)&p, sizeof p);
    LIFE(DROW(PEER).state == DNODE_DEAD, "a2s DEAD@5 + its own ALIVE@5: stays DEAD");
    LIFE(gq_find_incarn(PEER, DNODE_DEAD) == 5, "a2q DEAD@5 is queued (told)");

    LIFE_SET(DNODE_DEAD, 5);
    life_pkt(&p, PEER, 1, PEER, DNODE_ALIVE, 6); swim_rx(0x0A010006UL, SWIM_PORT, (const UB *)&p, sizeof p);
    LIFE(DROW(PEER).state == DNODE_ALIVE && SWT_INC(PEER) == 6, "b DEAD@5 + its own ALIVE@6: ALIVE at 6");

    LIFE_SET(DNODE_DEAD, 5);
    swim_probe_ok(PEER, " -> ALIVE (life test probe)\r\n");
    LIFE(DROW(PEER).state == DNODE_DEAD, "cp DEAD@5 + a probe answer: stays DEAD");
    LIFE_SET(DNODE_DEAD, 5);
    {
        UB *hbb = (UB *)&hb;
        for (INT i = 0; i < (INT)sizeof hb; i++) hbb[i] = 0;
        hb.magic = DRPC_MAGIC; hb.version = DRPC_VERSION; hb.type = DRPC_HEARTBEAT;
        hb.src_node = PEER; hb.dst_node = 0xFF;
        drpc_rx(0x0A010006UL, DRPC_PORT, (const UB *)&hb, sizeof hb);
    }
    LIFE(DROW(PEER).state == DNODE_DEAD, "ch DEAD@5 + a drpc HEARTBEAT: stays DEAD");

    LIFE_SET(DNODE_DEAD, 255);
    life_pkt(&p, THIRD, 1, PEER, DNODE_ALIVE, 0); gossip_apply(&p);
    LIFE(DROW(PEER).state == DNODE_ALIVE, "d DEAD@255 + rumour ALIVE@0: ALIVE");

    LIFE_SET(DNODE_ALIVE, 0);
    life_pkt(&p, THIRD, 1, PEER, DNODE_DEAD, 255); gossip_apply(&p);
    LIFE(DROW(PEER).state == DNODE_ALIVE, "e ALIVE@0 + rumour DEAD@255: ignored");

    {
        pk_node_t me = drpc_my_node;
        my_incarnation = 200; gq_cnt = 0;
        life_pkt(&p, THIRD, 1, (UB)me, DNODE_DEAD, 255); gossip_apply(&p);
        LIFE(my_incarnation == 0, "f0 my refute of DEAD@255 is 0");
        LIFE(gossip_supersedes(my_incarnation, DNODE_ALIVE, 255, DNODE_DEAD), "fb ALIVE@0 beats DEAD@255");
        my_incarnation = saved_inc;
    }

    LIFE_SET(DNODE_SUSPECT, 5);
    life_pkt(&p, PEER, 0, 0, 0, 0); swim_rx(0x0A010006UL, SWIM_PORT, (const UB *)&p, sizeof p);
    LIFE(DROW(PEER).state == DNODE_ALIVE, "g SUSPECT@5 + a packet from it: ALIVE");
#undef LIFE
#undef LIFE_SET

    drpc_my_node = saved_my; my_incarnation = saved_inc; gq_cnt = saved_gq;
    DROWW(PEER).state = saved_st; SWT_INC(PEER) = saved_pinc; DROWW(PEER).ip = saved_ip;
    SWT_SUS(PEER) = 0;
    say(fails ? "[swim-life] FAIL\r\n" : "[swim-life] PASS\r\n");
    SW_SELFTEST_LEAVE();
    return fails;
}
#endif /* _TK_HOSTED_LIBC_ (N2c stage 2, nodes life) */
