/*
 *  b5_gc_slots_test.c — B5 gc-restart step 2 (research/b5-gc-restart.md §4c,
 *  2026-11-25, written before the code, after audit-231 🔁GCR). The cleanup's
 *  roots (gossip_learn.c gl_gc_*) driven by hand on a real durable store; the
 *  test plays the peers (gl_gc_test_*: only with -DGL_GC_TEST, never in a
 *  product build). Every generation is a 10,000-byte blob of its own random
 *  bytes, published with the publish's own note muted.
 *
 *  Checks:
 *    [gcs-full]     16 peers fill the 16 slots; a 17th gets none while the
 *                   others are fresh (no slot is taken from a peer in use)
 *    [gcs-evict]    after peer 1 has gone 64+ changes without one, the 17th
 *                   gets a slot, peer 1 loses its own and its generation is
 *                   gone from the store (0 of its ids here)
 *    [gcs-17th]     the 17th's next generation drops its previous one (the
 *                   17th peer of a store's life is cleaned)
 *    [gcs-fresh]    the kept generations of peers 2..16 are whole after it
 *    [gcs-pend]     a fetch cut by a restart: what came of it is dropped when
 *                   the roots are read back, the kept generation stays whole
 *    [gcs-pend2]    a fetch that did not finish is dropped when the next
 *                   generation of the same peer starts to be fetched
 *    [gcs-holds]    2b: a kept generation is held for the boot SYNC (pfs_repl.c
 *                   does not stream it), a dropped one is not
 *    [gcs-lost]     a kept generation whose index was lost (a power loss) is
 *                   forgotten and the cleanup goes on (before: "cannot be
 *                   read; nothing dropped" for every cleanup after)
 *    [gcs-vic]      audit-232, test first (red on ad94eb1f): a kill after a
 *                   note's gl.gcref write, before its drop, leaves the victim
 *                   on no root; it must be dropped after the restart
 *    [gcs-vic3]     audit-233: the restart's drop of a victim builds the guard
 *                   first (a chunk shared with a kept generation stays)
 *
 *  Falsifiers (tests/llm/run_b5_gc_slots.sh): -DB5_NC_GC_NO_EVICT turns
 *  gcs-evict and gcs-17th red; -DB5_NC_GC_NO_PEND gcs-pend and gcs-pend2;
 *  -DB5_NC_GC_KEEP_LOST gcs-lost, gcs-holds, gcs-vic, gcs-vic2 and gcs-vic3 (no guard table);
 *  -DB5_NC_GC_NO_VIC gcs-vic and gcs-vic3 (step 3: the victim is not in gl.gcref).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned short node_t;
extern int  gl_student_publish(node_t node, const void *blob, unsigned int len);
extern int  pfs_durable_restore(void (*emit)(const char *));
extern int  pfs_durable_active(void);
extern void pfs_repl_init(void);
extern int  pfs_drop(const unsigned char id[32]);
extern int  pfs_has(const unsigned char id[32]);
extern void pfs_id_compute(const void *buf, unsigned int len, unsigned char id[32]);
extern void gl_gc_test_mute(int on);
extern void gl_gc_test_cut_next(void);
extern void gl_gc_test_cut_leaf_next(void);
extern unsigned gl_gc_test_shorts(void);
extern void gl_gc_test_note(node_t node, const unsigned char root[32], unsigned depth, unsigned nchunk);
extern void gl_gc_test_pend(node_t node, const unsigned char root[32], unsigned depth, unsigned nchunk);
extern void gl_gc_test_restart(void);
extern int  gl_gc_test_slot(node_t node);
extern void gl_gc_test_count(const unsigned char root[32], unsigned depth, unsigned nchunk,
                             unsigned *present, unsigned *seen);
extern void gl_gc_test_desc(unsigned char root[32], unsigned *depth, unsigned *nchunk);
extern int  gl_gc_holds(const unsigned char id[32]);

static int g_pass, g_fail;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

static void emit(const char *s) { fputs(s, stdout); }

#define BLOB_LEN 10000u
#define CHUNK 4096u              /* GL_ST_CHUNK = PFS_BLOCK_MAX */
#define NCH ((BLOB_LEN + CHUNK - 1) / CHUNK)
#define BIG_NCH 300u             /* [gcs-vic4]: 3 leaves of 127, 127, 46 = depth 2 */
#define BIG_LEN (BIG_NCH * CHUNK - 100u)
typedef struct { unsigned char root[32]; unsigned depth, nchunk; unsigned char cid[NCH][32]; } gen_t;
static unsigned g_seed = 0x6c5u;

static unsigned char blob[BLOB_LEN];
static unsigned g_keep;              /* audit-233: bytes kept from the last blob (a shared chunk) */
static gen_t make_gen(void)          /* a fresh generation in the store */
{
    for (unsigned i = g_keep; i < BLOB_LEN; i++) { g_seed = g_seed * 1103515245u + 12345u; blob[i] = (unsigned char)(g_seed >> 16); }
    gen_t g; memset(&g, 0, sizeof g);
    if (gl_student_publish(200, blob, BLOB_LEN) <= 0) { printf("[setup] publish failed\n"); exit(2); }
    gl_gc_test_desc(g.root, &g.depth, &g.nchunk);
    if (g.nchunk != NCH) { printf("[setup] nchunk %u, not %u\n", g.nchunk, NCH); exit(2); }
    for (unsigned c = 0; c < NCH; c++) {
        unsigned off = c * CHUNK, len = BLOB_LEN - off < CHUNK ? BLOB_LEN - off : CHUNK;
        pfs_id_compute(blob + off, len, g.cid[c]);
    }
    return g;
}
/* blocks of g still in the store: its chunks (ids computed here from the
 * bytes, not read through its index) and its index block */
static unsigned here(const gen_t *g)
{
    unsigned p = pfs_has(g->root) ? 1u : 0u;
    for (unsigned c = 0; c < NCH; c++) if (pfs_has(g->cid[c])) p++;
    return p;
}
static int whole(const gen_t *g)
{
    unsigned p = 0, s = 0;
    gl_gc_test_count(g->root, g->depth, g->nchunk, &p, &s);
    return s == NCH + 1u && p == s && here(g) == NCH + 1u;
}

int main(void)
{
    printf("=== B5 gc-restart step 2: slots, fetches cut short, lost generations ===\n");
    pfs_durable_restore(emit);
    if (!pfs_durable_active()) { printf("[setup] FAIL durable store not active (set PKERNEL_PFS_DIR)\n"); return 2; }
    pfs_repl_init();
    gl_gc_test_mute(1);

    static gen_t cur[18];
    for (node_t k = 1; k <= 16; k++) { cur[k] = make_gen(); gl_gc_test_note(k, cur[k].root, cur[k].depth, cur[k].nchunk); }
    gen_t g17a = make_gen();
    gl_gc_test_note(17, g17a.root, g17a.depth, g17a.nchunk);
    printf("[gcs] after 16 peers: slot of 17 %d, of 1 %d\n", gl_gc_test_slot(17), gl_gc_test_slot(1));
    CHECK(gl_gc_test_slot(17) < 0 && gl_gc_test_slot(1) >= 0 && whole(&cur[1]),
          "[gcs-full] a 17th peer gets no slot while the 16 are fresh");

    /* peers 2..16 move on 5 times (75 changes); peer 1 does not */
    for (int round = 0; round < 5; round++)
        for (node_t k = 2; k <= 16; k++) {
            gen_t g = make_gen();
            gl_gc_test_note(k, g.root, g.depth, g.nchunk);
            cur[k] = g;
        }
    gen_t g17b = make_gen();
    gl_gc_test_note(17, g17b.root, g17b.depth, g17b.nchunk);
    unsigned h1 = here(&cur[1]);
    printf("[gcs] after 75 changes: slot of 17 %d, of 1 %d, peer 1's generation %u ids here\n",
           gl_gc_test_slot(17), gl_gc_test_slot(1), h1);
    CHECK(gl_gc_test_slot(17) >= 0 && gl_gc_test_slot(1) < 0 && h1 == 0,
          "[gcs-evict] a slot unchanged for 64+ changes goes to the 17th, its generation dropped");

    gen_t g17c = make_gen();
    gl_gc_test_note(17, g17c.root, g17c.depth, g17c.nchunk);
    unsigned hb = here(&g17b);
    printf("[gcs] 17's previous generation: %u ids here, its new one whole %d\n", hb, whole(&g17c));
    CHECK(hb == 0 && whole(&g17c), "[gcs-17th] the 17th peer's previous generation is dropped");

    int fresh = 1;
    for (node_t k = 2; k <= 16; k++) if (!whole(&cur[k])) fresh = 0;
    CHECK(fresh, "[gcs-fresh] the kept generations of peers 2..16 are whole");

    /* a fetch cut short: peer 2's next generation is pending, then a restart */
    gen_t p2 = make_gen();
    gl_gc_test_pend(2, p2.root, p2.depth, p2.nchunk);
    gl_gc_test_restart();
    gen_t g3 = make_gen();               /* any change reads the roots back */
    gl_gc_test_note(3, g3.root, g3.depth, g3.nchunk);
    unsigned hp = here(&p2);
    printf("[gcs] after a restart: the cut fetch %u ids here, peer 2's kept whole %d, slot of 2 %d\n",
           hp, whole(&cur[2]), gl_gc_test_slot(2));
    CHECK(hp == 0 && whole(&cur[2]) && gl_gc_test_slot(2) >= 0,
          "[gcs-pend] a fetch cut by a restart is dropped, the kept generation stays");
    cur[3] = g3;

    /* a fetch that gave up: peer 4 starts A, then B; A goes, B and the kept stay */
    gen_t a4 = make_gen(), b4 = make_gen();
    gl_gc_test_pend(4, a4.root, a4.depth, a4.nchunk);
    gl_gc_test_pend(4, b4.root, b4.depth, b4.nchunk);
    unsigned ha = here(&a4);
    int bw = whole(&b4), kw = whole(&cur[4]);
    gl_gc_test_note(4, b4.root, b4.depth, b4.nchunk);
    printf("[gcs] peer 4: the unfinished fetch %u ids here, the next whole %d, the kept whole %d,"
           " after its note the old kept %u ids here\n", ha, bw, kw, here(&cur[4]));
    CHECK(ha == 0 && bw && kw && here(&cur[4]) == 0 && whole(&b4),
          "[gcs-pend2] a fetch that did not finish is dropped at the next one");
    cur[4] = b4;

    /* a lost index: peer 5's kept root index is gone (a power loss) */
    pfs_drop(cur[5].root);
    gen_t g6 = make_gen();
    gen_t old6 = cur[6];
    gl_gc_test_note(6, g6.root, g6.depth, g6.nchunk);
    unsigned h6 = here(&old6);
    gen_t g7 = make_gen(); gen_t old7 = cur[7];
    gl_gc_test_note(7, g7.root, g7.depth, g7.nchunk);
    unsigned h7 = here(&old7);
    printf("[gcs] peer 5's index lost: peer 6's old generation %u ids here, peer 7's %u\n", h6, h7);
    CHECK(h6 == 0 && h7 == 0 && whole(&g6) && whole(&g7),
          "[gcs-lost] a kept generation that cannot be read is forgotten, the cleanup goes on");

    /* 2b: the boot-SYNC responder asks gl_gc_holds (pfs_repl.c) */
    int hk = 0, hd = 0;
    for (unsigned c = 0; c < NCH; c++) { hk += gl_gc_holds(cur[8].cid[c]) == 1; hd += gl_gc_holds(old7.cid[c]) == 1; }
    hk += gl_gc_holds(cur[8].root) == 1; hd += gl_gc_holds(old7.root) == 1;
    printf("[gcs] held for the boot SYNC: a kept generation %d of %u ids, a dropped one %d\n", hk, NCH + 1u, hd);
    CHECK(hk == (int)NCH + 1 && hd == 0, "[gcs-holds] a kept generation is held (not streamed by a boot SYNC), a dropped one is not");

    /* audit-232 (test first, red on ad94eb1f): a kill after the note's
     * gl.gcref write and before its drop. Live: run_b5_gc.sh B5GC_PHASE=h3q,
     * 1 arm of 4 R1 FAIL 1.057 = node 1 kept st/3's gen 2 for good (gen 3's
     * fetch had all its chunks; gen 3 was the root after the restart). The
     * victim must be dropped after the restart, not stay on no root. */
    gen_t old9 = cur[9], v9 = make_gen();
    gl_gc_test_cut_next();
    gl_gc_test_note(9, v9.root, v9.depth, v9.nchunk);
    unsigned hc = here(&old9);            /* the "kill" came before the drop */
    gl_gc_test_restart();
    gen_t g10 = make_gen();               /* any change reads the roots back */
    gl_gc_test_note(10, g10.root, g10.depth, g10.nchunk);
    cur[10] = g10;
    unsigned hr = here(&old9);
    gen_t w9 = make_gen();                /* and peer 9 moves on once more */
    gl_gc_test_note(9, w9.root, w9.depth, w9.nchunk);
    unsigned hn = here(&old9);
    printf("[gcs] a kill between the write and the drop: peer 9's victim %u ids here at the kill,"
           " %u after the restart, %u after peer 9's next generation\n", hc, hr, hn);
    CHECK(hc == NCH + 1u && hr == 0 && hn == 0 && whole(&w9),
          "[gcs-vic] a victim the kill left between the write and the drop is dropped after the restart");

    /* step 3, live h3q 3 arms of 4 on 64870212: the drop was done before the
     * kill, but the file still named the victim (cleared in RAM only); the
     * restart walked a gone generation and printed "not all here" (G2 red).
     * A victim already dropped is forgotten without a short walk. */
    gen_t old11 = cur[11], v11 = make_gen();
    gl_gc_test_note(11, v11.root, v11.depth, v11.nchunk);   /* drop done, no later write */
    cur[11] = v11;
    unsigned hd11 = here(&old11);
    unsigned s0 = gl_gc_test_shorts();
    gl_gc_test_restart();
    gen_t g12 = make_gen();
    gl_gc_test_note(12, g12.root, g12.depth, g12.nchunk);
    cur[12] = g12;
    unsigned s1 = gl_gc_test_shorts();
    printf("[gcs] a victim dropped before the kill: %u ids here, short walks after the restart %u\n", hd11, s1 - s0);
    CHECK(hd11 == 0 && s1 == s0 && whole(&v11) && whole(&g12),
          "[gcs-vic2] a victim dropped before the kill is forgotten after the restart without a short walk");

    /* audit-233: the restart's drop of a named victim builds the guard first.
     * Peer 14's victim a14 shares its first chunk with peer 13's kept k13 (two
     * generations of one model share the blocks of the weights that did not
     * move). After the restart that chunk must stay (k13 whole), the rest of
     * a14 must go. Before this, gl_gc_test_restart kept the last guard table,
     * so a drop without its own guard still spared the kept set. */
    gen_t k13 = make_gen();
    gl_gc_test_note(13, k13.root, k13.depth, k13.nchunk);
    cur[13] = k13;
    g_keep = CHUNK;                       /* a14's first chunk = k13's */
    gen_t a14 = make_gen();
    g_keep = 0;
    int shared = memcmp(a14.cid[0], k13.cid[0], 32) == 0;
    gl_gc_test_note(14, a14.root, a14.depth, a14.nchunk);
    cur[14] = a14;
    gen_t b14 = make_gen();
    gl_gc_test_cut_next();
    gl_gc_test_note(14, b14.root, b14.depth, b14.nchunk);   /* "killed" before a14's drop */
    cur[14] = b14;
    unsigned va = here(&a14);
    gl_gc_test_restart();
    gen_t g15 = make_gen();               /* any change reads the roots back */
    gl_gc_test_note(15, g15.root, g15.depth, g15.nchunk);
    cur[15] = g15;
    unsigned vb = here(&a14);
    int vk = whole(&k13), vw = whole(&b14);
    printf("[gcs] a victim sharing a chunk with a kept generation: shared %d, victim %u ids here at the kill,"
           " %u after the restart (1 = the shared chunk), the kept whole %d, peer 14's new whole %d\n",
           shared, va, vb, vk, vw);
    CHECK(shared && va == NCH + 1u && vb == 1u && vk && vw,
          "[gcs-vic3] the restart's drop of a victim spares the chunk a kept generation shares");

    /* limit 1 of step 3 (research/b5-gc-restart.md §4d, test first): a depth-2
     * victim (3 leaves), "killed" during its drop right after the first leaf
     * index went (the walk drops a leaf's chunks, then the leaf, the root
     * last). The restart's walk must not stop at the gone leaf: the chunks of
     * leaves 2 and 3 and the root go too. */
    static unsigned char bigb[BIG_LEN];
    for (unsigned i = 0; i < BIG_LEN; i++) { g_seed = g_seed * 1103515245u + 12345u; bigb[i] = (unsigned char)(g_seed >> 16); }
    unsigned char broot[32]; unsigned bdepth = 0, bn = 0;
    if (gl_student_publish(200, bigb, BIG_LEN) <= 0) { printf("[setup] big publish failed\n"); return 2; }
    gl_gc_test_desc(broot, &bdepth, &bn);
    static unsigned char bcid[BIG_NCH][32];
    for (unsigned c = 0; c < BIG_NCH; c++) {
        unsigned off = c * CHUNK, len = BIG_LEN - off < CHUNK ? BIG_LEN - off : CHUNK;
        pfs_id_compute(bigb + off, len, bcid[c]);
    }
    gl_gc_test_note(16, broot, bdepth, bn);       /* peer 16 keeps the big one */
    gen_t n16 = make_gen();
    gl_gc_test_cut_leaf_next();
    gl_gc_test_note(16, n16.root, n16.depth, n16.nchunk);   /* "killed" after leaf 1's drop */
    cur[16] = n16;
    unsigned bk = 0, ba = 0;
    for (unsigned c = 0; c < BIG_NCH; c++) bk += pfs_has(bcid[c]) != 0;
    gl_gc_test_restart();
    gen_t g2b = make_gen();               /* any change reads the roots back */
    gl_gc_test_note(2, g2b.root, g2b.depth, g2b.nchunk);
    cur[2] = g2b;
    for (unsigned c = 0; c < BIG_NCH; c++) ba += pfs_has(bcid[c]) != 0;
    int broot_here = pfs_has(broot) != 0;
    printf("[gcs] a depth-%u victim of %u chunks killed after its first leaf: %u chunks here at the kill,"
           " %u after the restart, its root here %d, peer 16's new whole %d\n",
           bdepth, bn, bk, ba, broot_here, whole(&n16));
    CHECK(bdepth == 2 && bn == BIG_NCH && bk == BIG_NCH - 127u && ba == 0 && !broot_here && whole(&n16),
          "[gcs-vic4] a depth-2 victim killed during its drop is dropped whole after the restart (the walk goes past a gone leaf)");

    printf("[gcs] %d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
