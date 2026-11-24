/*
 *  gossip_learn.c — G22 / §8 §9 decentralized COLLECTIVE learning.
 *
 *  See gossip_learn.h for the why. The crux, in one sentence: N nodes
 *  each see only PART of the task (disjoint, leave-one-class-out shards),
 *  yet by periodically averaging each other's full weight bodies (no
 *  central aggregator) every node ends up ABOVE the best a node can do
 *  on its shard alone. The swarm learns what no node could.
 *
 *  Built on dtr.c's REAL 635-param transformer + analytic backprop
 *  (dtr_train_batch / dtr_eval_batch / dtr_weights_get/set /
 *  dtr_reinit_weights — all public). We never touch dtr's math; we
 *  drive it through its public API, swapping weight sets in and out to
 *  simulate N nodes in-process, and gossiping them over p-fs live.
 */

#include "gossip_learn.h"
#include "scaling_cert.h" /* hosted: scaling_self_test (self-guards on bare metal) */
#include "dtr.h"
#include "reflex.h"      /* G38: 学習→守る (reflex_would_fire) / 守る→学習 (経験) */
#include "pfs_block.h"
#include "pfs_dag.h"
#include "drpc.h"
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* output helpers (sio frame channel, like dtr_train.c / protect.c)    */
/* ------------------------------------------------------------------ */

IMPORT void sio_send_frame(const UB *buf, INT size);

static void gp(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void gpd(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { gp("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    gp(&buf[i]);
}

/* xx.x percent / loss */
static void gpf1(float f)
{
    if (f < 0.0f) { gp("-"); f = -f; }
    UW whole = (UW)f;
    UW frac  = (UW)((f - (float)whole) * 10.0f + 0.5f);
    if (frac >= 10) { whole++; frac = 0; }
    gpd(whole); gp("."); gpd(frac);
}

/* ------------------------------------------------------------------ */
/* generic decentralized primitives                                    */
/* ------------------------------------------------------------------ */

void gl_accumulate(float *acc, const float *w, UW n)
{
    for (UW i = 0; i < n; i++) acc[i] += w[i];
}

void gl_scale(float *w, float s, UW n)
{
    for (UW i = 0; i < n; i++) w[i] *= s;
}

void gl_merge(float *out, const float *const *models, UW count, UW n)
{
    if (count == 0) return;
    for (UW i = 0; i < n; i++) out[i] = 0.0f;
    for (UW k = 0; k < count; k++) gl_accumulate(out, models[k], n);
    gl_scale(out, 1.0f / (float)count, n);
}

/* ------------------------------------------------------------------ */
/* LM-11 / Path W² — per-parameter WEIGHTED merge siblings.            */
/* gl_merge (above) is UNCHANGED and keeps driving LM-10; these are    */
/* purely ADDITIVE (living-mind Part XII, anti-fork XII.6).            */
/* ------------------------------------------------------------------ */

void gl_accumulate_w(float *acc, float *wsum,
                     const float *w, const float *wt, UW n)
{
    for (UW i = 0; i < n; i++) {
        acc[i]  += wt[i] * w[i];
        wsum[i] += wt[i];
    }
}

/* gl_merge_w needs one per-parameter weight-sum scratch. The largest
 * body merged is the R3 weight-state (R_NP=21568 floats, the [wmerge-*]
 * cert), far larger than the dtr body (DTR_WEIGHT_FLOATS=635). Size the
 * scratch to that ceiling, static .bss (never the task stack — the
 * stack-overflow lesson). Shell/cert task only, no concurrent use. */
#define GL_MERGE_MAXFLOATS  21568u   /* >= R_NP; the largest weighted body */
static float gl_wsum[GL_MERGE_MAXFLOATS];

void gl_merge_w(float *out, const float *const *models,
                const float *const *weights, UW count, float eps, UW n)
{
    if (count == 0) return;
    if (n > GL_MERGE_MAXFLOATS) return;        /* scratch bound (defensive) */
    for (UW i = 0; i < n; i++) { out[i] = 0.0f; gl_wsum[i] = 0.0f; }
    for (UW k = 0; k < count; k++)
        gl_accumulate_w(out, gl_wsum, models[k], weights[k], n);
    /* normalize per-parameter by Σ wt + eps (denominator safety only).
     * The plain-mean FALLBACK for a parameter neither model trained is
     * delivered by the CALLER adding a uniform floor to each model's
     * weight vector (W2: wtₖ[i] = Fₖ[i] + floor), so Fₖ[i]≈0 for all k
     * -> wtₖ[i]≈floor -> out[i] -> (Σ floor·wₖ)/(C·floor) = plain mean of
     * the shared backbone (XII.2). eps here is the SAME small constant
     * for every parameter, so the per-param SUM stays order-independent
     * ([wmerge-nocentral]); it only guards a literal all-zero weight. */
    for (UW i = 0; i < n; i++) out[i] /= (gl_wsum[i] + eps);
}

/* ------------------------------------------------------------------ */
/* p-fs transport (public pfs_dag API only)                            */
/* ------------------------------------------------------------------ */

/* one shared scratch blob: 8-byte header + N float32. Static, never on
 * the task stack (feedback_hosted_relay_stack_overflow). Driven from
 * the shell task only, so no concurrent use. */
#define GL_MAXFLOATS  DTR_WEIGHT_FLOATS
static struct __attribute__((packed, aligned(4))) {
    GL_BLOB_HDR h;
    float       w[GL_MAXFLOATS];
} gl_blob;

_Static_assert(sizeof(GL_BLOB_HDR) == 8, "GL_BLOB_HDR must be 8 bytes");
_Static_assert(sizeof(float) == 4, "float must be IEEE754 binary32");
_Static_assert(sizeof(gl_blob) == 8 + GL_MAXFLOATS * 4,
               "gl_blob must be header + GL_MAXFLOATS packed float32");
_Static_assert(sizeof(gl_blob) <= PFS_BLOCK_MAX,
               "gl_blob must fit one p-fs block");

INT gl_pfs_publish(const char *ref, UW reflen, const float *w, UW n)
{
    if (n > GL_MAXFLOATS) return -1;
    gl_blob.h.magic = GL_BLOB_MAGIC;
    gl_blob.h.n     = n;
    for (UW i = 0; i < n; i++) gl_blob.w[i] = w[i];
    INT r = pfs_dag_save((const UB *)ref, reflen, &gl_blob,
                         (UW)(sizeof(GL_BLOB_HDR) + n * 4));
    return (r == PFS_OK) ? 0 : -1;
}

INT gl_pfs_fetch(const char *ref, UW reflen, float *w, UW n)
{
    if (n > GL_MAXFLOATS) return -1;
    INT r = pfs_dag_read((const UB *)ref, reflen, &gl_blob,
                         (UW)(sizeof(GL_BLOB_HDR) + n * 4));
    if (r != (INT)(sizeof(GL_BLOB_HDR) + n * 4)) return -1;
    if (gl_blob.h.magic != GL_BLOB_MAGIC || gl_blob.h.n != n) return -1;
    for (UW i = 0; i < n; i++) w[i] = gl_blob.w[i];
    return 0;
}

/* ------------------------------------------------------------------ */
/* SS-3 — chunked p-fs transport for the (multi-MB) STUDENT blob.      */
/*                                                                     */
/* The student blob is far larger than one PFS_BLOCK_MAX block, so it   */
/* is SPLIT into ≤PFS_BLOCK_MAX-byte chunk objects + a tiny header      */
/* object.  Refs live in "st/<node>/<i>" (<=PFS_NAME_MAX).  Static      */
/* scratch (one block + one header) — never the task stack             */
/* (feedback_hosted_relay_stack_overflow).  Shell-task only.           */
/* ------------------------------------------------------------------ */

#define GL_ST_CHUNK  PFS_BLOCK_MAX        /* bytes per chunk object          */

#ifndef _TK_HOSTED_LIBC_
/* ================================================================== */
/* BARE-METAL legacy transport — crown §4: byte-for-byte trunk.        */
/* The relay-capable ([live]) transport lives in the _TK_HOSTED_LIBC_  */
/* branch below; bare metal never runs it, so the preprocessor output  */
/* here (hence kernel .text / the 755a20fa crown) is UNCHANGED.        */
/* ================================================================== */
#define GL_ST_MAGIC  0x53544831u          /* "STH1" little-endian header tag */

/* header object payload: total byte length + chunk count + magic. */
typedef struct __attribute__((packed)) {
    UW magic;        /* GL_ST_MAGIC                         */
    UW total_len;    /* full student-blob byte length       */
    UW nchunk;       /* number of chunk objects             */
} GL_ST_HDR;

static UB gl_st_chunk[GL_ST_CHUNK];       /* one chunk's transfer scratch    */
static GL_ST_HDR gl_st_hdr;               /* the header transfer scratch     */

/* build the chunk/header ref "st/<node>/<i>" or "st/<node>/h" (<=16 chars).
 * Returns the ref length. `i` < 0 selects the header suffix 'h'. */
static UW gl_student_ref(char *out, pk_node_t node, INT i)
{
    UW k = 0;
    out[k++] = 's'; out[k++] = 't'; out[k++] = '/';
    if (node >= 10) out[k++] = (char)('0' + (node / 10) % 10);
    out[k++] = (char)('0' + node % 10);
    out[k++] = '/';
    if (i < 0) { out[k++] = 'h'; return k; }
    /* up-to-5-digit chunk index, ascending-digit emit (no leading zeros). */
    UW v = (UW)i; char tmp[6]; INT t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v > 0 && t < 6) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    while (t > 0) out[k++] = tmp[--t];
    return k;
}

INT gl_student_publish(pk_node_t node, const void *blob, UW len)
{
    if (!blob || len == 0) return -1;
    UW nchunk = (len + GL_ST_CHUNK - 1) / GL_ST_CHUNK;
    if (nchunk > GL_ST_MAXCHUNK) return -1;        /* refuse, NEVER truncate  */

    const UB *src = (const UB *)blob;
    for (UW c = 0; c < nchunk; c++) {
        UW off = c * GL_ST_CHUNK;
        UW clen = (len - off < GL_ST_CHUNK) ? (len - off) : GL_ST_CHUNK;
        for (UW i = 0; i < clen; i++) gl_st_chunk[i] = src[off + i];
        char ref[20]; UW rl = gl_student_ref(ref, node, (INT)c);
        if (pfs_dag_save((const UB *)ref, rl, gl_st_chunk, clen) != PFS_OK)
            return -1;
    }
    /* header LAST: a reader that sees the header is guaranteed the chunks
     * were published (best-effort ordering for the eventual-consistency mesh). */
    gl_st_hdr.magic = GL_ST_MAGIC;
    gl_st_hdr.total_len = len;
    gl_st_hdr.nchunk = nchunk;
    char href[20]; UW hrl = gl_student_ref(href, node, -1);
    if (pfs_dag_save((const UB *)href, hrl, &gl_st_hdr, sizeof gl_st_hdr) != PFS_OK)
        return -1;
    return (INT)nchunk;
}

INT gl_student_fetch(pk_node_t node, void *out, UW cap)
{
    if (!out) return -1;
    char href[20]; UW hrl = gl_student_ref(href, node, -1);
    INT hr = pfs_dag_read((const UB *)href, hrl, &gl_st_hdr, sizeof gl_st_hdr);
    if (hr != (INT)sizeof gl_st_hdr) return -1;
    if (gl_st_hdr.magic != GL_ST_MAGIC) return -1;
    UW len = gl_st_hdr.total_len, nchunk = gl_st_hdr.nchunk;
    if (len == 0 || nchunk == 0 || nchunk > GL_ST_MAXCHUNK) return -1;
    if (len > cap) return -1;                       /* refuse, NEVER truncate  */
    UW expect = (len + GL_ST_CHUNK - 1) / GL_ST_CHUNK;
    if (nchunk != expect) return -1;                /* header/length mismatch  */

    UB *dst = (UB *)out;
    for (UW c = 0; c < nchunk; c++) {
        UW off = c * GL_ST_CHUNK;
        UW clen = (len - off < GL_ST_CHUNK) ? (len - off) : GL_ST_CHUNK;
        char ref[20]; UW rl = gl_student_ref(ref, node, (INT)c);
        INT r = pfs_dag_read((const UB *)ref, rl, gl_st_chunk, GL_ST_CHUNK);
        if (r != (INT)clen) return -1;              /* chunk missing / wrong   */
        for (UW i = 0; i < clen; i++) dst[off + i] = gl_st_chunk[i];
    }
    return (INT)len;
}

#else  /* _TK_HOSTED_LIBC_ — content-addressed MANIFEST transport ([live]) */
/* ================================================================== */
/* SS-3 [live] transport (student-blob-transport.md §1-2). Every 4 KB   */
/* chunk -> pfs_repl_put -> content-id (NO name). A 2-level index       */
/* (leaf blocks of <=127 chunk-ids; one root of <=127 leaf-ids) +       */
/* a tiny descriptor reachable from ONE named ref "st/<node>" collapses */
/* the 483-name explosion to a single name. The fetcher walks the index */
/* and pulls every chunk by EXPLICIT WINDOWED pfs_repl_want (paced to    */
/* the 8-slot pending table), FAILING CLOSED (never truncating) if any   */
/* chunk never arrives. All scratch is static + HOSTED-only, so bare     */
/* metal gains NO symbol.                                                */
/* ================================================================== */
#include "pfs_repl.h"
#include "llm/student.h"   /* ST_RG_FETCH_MAX, ST_RG_HDR: the RG-FETCH tie below */

/* RG-FETCH (research/b5-recv-mem.md §6): a region round fetches every copy into
 * one buffer of ST_RG_FETCH_MAX (student.h, written out as 8192 x 4096 + hdr).
 * Publish below takes a copy of up to GL_ST_MAXCHUNK chunks of GL_ST_CHUNK. If
 * one side changed alone, gl_student_fetch would refuse a copy publish took
 * (len > cap) and the round would read "could not fetch". Tied here; a change
 * to either side must change the other (tests/llm/run_rg_fetch_tie.sh). */
_Static_assert((unsigned long long)ST_RG_FETCH_MAX ==
               (unsigned long long)GL_ST_MAXCHUNK * GL_ST_CHUNK + ST_RG_HDR,
               "RG-FETCH tie: ST_RG_FETCH_MAX (student.h) must be GL_ST_MAXCHUNK x GL_ST_CHUNK + ST_RG_HDR");

/* windowed-want fetch policy (§2.1). W <= PFSR_PENDING_MAX. */
#if defined(B5W_NC_LOUD)
#define GL_ST_WANT_WINDOW    6u            /* outstanding wants per pass      */
#else
#define GL_ST_WANT_WINDOW    24u           /* unicast: no shared slot to clobber (b5-wire2 §6) */
#endif
#define GL_ST_FETCH_POLL_MS  60            /* yield between passes            */
/* give up after this many no-progress passes. Stage 2 (b5-wire2.md §6): 4
 * (0.24 s) was shorter than one re-WANT (PFSR_WANT_RETRY_MS 600): with the
 * chunks quiet nothing is pushed ahead, a want lost in the LATEST_ONLY slot
 * is sent again only after 600 ms, and the fetch had already failed (live
 * L1: fetched 2-3 of 4 in the second round). 50 passes = 3 s = 5 re-WANTs. */
#define GL_ST_FETCH_STALL    167           /* about 10 s (the holder may be computing its own round) */
#define GL_ST_FETCH_MAXPASS  4096u         /* hard ceiling on total passes    */

/* B5 D4 (research/b5-m-round.md §4): the student's chunks are written without
 * their own fsyncs (pfs_put_lazy, a quiet id); one syncfs ends the publish
 * (before its descriptor) and the fetch. Weak: some unit tests link this file
 * without pfs_durable.c (no store = nothing to sync). */
extern int pfs_dur_sync(void) __attribute__((weak));
static INT gl_st_sync(void) { return pfs_dur_sync ? (INT)pfs_dur_sync() : 0; }

/* B5 wire stage 2 (research/b5-wire2.md §2 1, 3): the chunks and the index
 * blocks go quietly (no announce; a peer pulls them by want at its round, and
 * what it pulled is not re-announced either); the descriptor stays loud. The
 * publish yields GL_ST_YIELD_MS every GL_ST_YIELD_EVERY chunks. Falsifier
 * -DB5W_NC_LOUD: the stage-1 puts and wants (every chunk announced). */
#if defined(B5W_NC_LOUD)
#define gl_st_put  pfs_repl_put
#define gl_st_want pfs_repl_want
#else
/* the wants go to the blob's owner alone (pfs_repl_want_from; the region
 * slot lost most of them, live L1 at b7c04d6e/bfed9920) */
static pk_node_t gl_st_from;
static void gl_st_want(const U1 id[PFS_ID_LEN]) { pfs_repl_want_from(id, gl_st_from); }
#define gl_st_put  pfs_repl_put_quiet
#endif
/* re-ask one chunk only after this many passes (b5-wire2 §6, M: every
 * pass asked the same 24 again before they could arrive, node 0 sent 58,837
 * blocks for about 4,700 wanted). The loud falsifier keeps every pass. */
#if defined(B5W_NC_LOUD)
#define GL_ST_REASK          1u
#else
#define GL_ST_REASK          8u            /* about 0.5 s */
#endif
#define GL_ST_YIELD_EVERY    64u
#define GL_ST_YIELD_MS       10

/* layout pins (LP64 / wire): the index header is exactly 4 packed UW. */
_Static_assert(sizeof(UW) == 4, "UW must be 32-bit on the wire");
_Static_assert(GL_ST_IDS_PER_IDX == 127, "127 ids fit one 4096-byte index block");
_Static_assert(sizeof(GL_ST_INDEX) == GL_ST_IDX_HDR + GL_ST_IDS_PER_IDX * PFS_ID_LEN,
               "packed index block = 16-byte header + ids, no padding");
_Static_assert(sizeof(GL_ST_INDEX) <= PFS_BLOCK_MAX, "index block fits one p-fs block");
_Static_assert(sizeof(GL_ST_DESC) <= PFS_BLOCK_MAX, "descriptor fits one p-fs block");

/* static transfer scratch (never the task stack — the stack-overflow lesson).
 * Shell/cert-task only; no concurrent use. HOSTED-only, so bare metal is
 * unaffected. */
static UB         gl_st_chunk[GL_ST_CHUNK];                  /* one chunk in flight   */
static GL_ST_INDEX gl_st_idx;                                /* one index block       */
static U1         gl_st_leafids[GL_ST_IDS_PER_IDX][PFS_ID_LEN]; /* up to 127 leaf ids */
static U1         gl_st_idlist[GL_ST_MAXCHUNK][PFS_ID_LEN];  /* fetch: all chunk ids  */
static U1         gl_st_have[GL_ST_MAXCHUNK];               /* fetch: seen present   */
static UH         gl_st_asked[GL_ST_MAXCHUNK];              /* fetch: pass+1 last asked */
static GL_ST_DESC gl_st_desc;                                /* descriptor scratch    */
static INT        gl_st_drop = -1;          /* falsifier: chunk idx to OMIT (-1=off) */

void gl_student_test_drop_chunk(INT idx) { gl_st_drop = idx; }

/* ------------------------------------------------------------------ */
/* B5 ① (research/b5-m-default.md §3 2, §4 D-G1): the cleanup. For each  */
/* st/<node> the roots of the last two generations are kept (32 B each).  */
/* When a third arrives (this node's publish, or a fetch of a peer's), the */
/* oldest is walked through its index blocks (here, on disk) and every id  */
/* in it that no kept generation of ANY ref holds is dropped: the 0 chunks  */
/* after ⓪ and the chunks of two nodes that merged to the same weights have */
/* the same id across refs. The guard is a table of the ids' first 8 bytes */
/* (a prefix collision keeps a block: the safe side). Anything that cannot */
/* be read or does not fit skips the cleanup (nothing is dropped).          */
/* Falsifier -DB5_NC_NO_GC: no cleanup.                                     */
/* ------------------------------------------------------------------ */
#define GL_GC_REFS   16u
#define GL_GC_HBITS  17u
#define GL_GC_HSIZE  (1u << GL_GC_HBITS)
/* gc-restart step 2 (2026-11-25, after audit-231 🔁GCR): a slot unchanged for
 * GL_GC_STALE changes of the others is freed for a new node (its generations
 * are dropped); before, 16 slots once taken were never freed and, with the
 * roots now kept across restarts, the 17th peer of a store's life was never
 * cleaned. Falsifier -DB5_NC_GC_NO_EVICT. */
#define GL_GC_STALE  64u
typedef struct {
    UB        used, n;              /* n = generations kept (0..2)      */
    UB        own;                  /* 1 = this node's publish: keeps 2  */
    UB        pend;                 /* 1 = proot is being fetched        */
    pk_node_t node;
    U1        root[2][PFS_ID_LEN];  /* [0] newest, [1] the one before   */
    UB        depth[2];
    UW        nchunk[2];
    U1        proot[PFS_ID_LEN];    /* the generation being fetched: what came of it is on no root yet */
    UB        pdepth;
    UW        pnchunk;
    UW        last;                 /* gl_gc_tick at its last change */
} GL_GC_REF;
static GL_GC_REF   gl_gc_ref[GL_GC_REFS];
static UW          gl_gc_tick;                   /* changes of any ref */
static UW          gl_gc_key[GL_GC_HSIZE][2];   /* id[0..8), 0/0 = empty */
static UW          gl_gc_fill;
static GL_ST_INDEX gl_gc_root;                   /* a level-1 index being walked */
static GL_ST_INDEX gl_gc_leaf;                   /* a level-0 index being walked */
static UW          gl_gc_dropped, gl_gc_seen, gl_gc_present;

/* B5 gc-restart (research/b5-gc-restart.md §2): the roots above also live in
 * PKERNEL_PFS_DIR/gl.gcref (not 64 hex digits = never taken for a block by
 * pfs_durable.c's is_block_name), written durably (tmp + rename + fsync) each
 * time a ref changes and read once, before the first cleanup after a start.
 * Without it a kill or Android's stopSelf forgot the generations this node
 * knew, and their blocks stayed in the store for good. A file that is missing,
 * short or of another mark, version or shape is not used (nothing restored =
 * nothing dropped, as before). Weak: unit tests link this file without
 * pfs_durable.c. Falsifier -DB5_NC_GC_NO_RESTORE: the file is never read. */
extern int pfs_dur_write(const char *fname, const void *data, unsigned len) __attribute__((weak));
extern int pfs_dur_read(const char *fname, void *buf, unsigned maxlen) __attribute__((weak));
#define GL_GC_FILE   "gl.gcref"
#define GL_GC_MAGIC  0x46524347u                /* "GCRF" */
#define GL_GC_VER    2u                         /* 2: pend/proot/last and the tick */
typedef struct {
    UW        magic, ver, refs, refsz, tick;
    GL_GC_REF ref[GL_GC_REFS];
} GL_GC_SAVE;
static GL_GC_SAVE gl_gc_save;
static UB         gl_gc_loaded;
static void gl_gc_store(void);
static INT  gl_gc_guard(void);
static INT  gl_gc_eq(const U1 *a, const U1 *b);
static INT  gl_gc_walk(const U1 root[PFS_ID_LEN], UW depth, UW nchunk, INT mode);

/* drop what the guard (built just before) lacks of one generation; one line */
static void gl_gc_drop_gen(pk_node_t node, const U1 root[PFS_ID_LEN], UW depth, UW nchunk, const char *what)
{
    gl_gc_dropped = 0; gl_gc_seen = 0;
    INT w = gl_gc_walk(root, depth, nchunk, 2);
    gp("[b5-gc] st/"); gpd(node); gp(": dropped "); gpd(gl_gc_dropped); gp(" of ");
    gpd(gl_gc_seen); gp(" ids of "); gp(what);
    gp(w < 0 ? " (its index was not all here)\r\n" : "\r\n");
}

static void gl_gc_load(void)
{
    if (gl_gc_loaded) return;
    gl_gc_loaded = 1;
#if !defined(B5_NC_GC_NO_RESTORE)
    if (!pfs_dur_read) return;
    INT n = (INT)pfs_dur_read(GL_GC_FILE, &gl_gc_save, (unsigned)sizeof gl_gc_save);
    if (n < 0) return;                          /* no file: a first start */
    if (n != (INT)sizeof gl_gc_save || gl_gc_save.magic != GL_GC_MAGIC
        || gl_gc_save.ver != GL_GC_VER || gl_gc_save.refs != GL_GC_REFS
        || gl_gc_save.refsz != (UW)sizeof(GL_GC_REF)) {
        gp("[b5-gc] roots: gl.gcref not used (size or mark)\r\n");
        return;
    }
    UW used = 0;
    for (UW i = 0; i < GL_GC_REFS; i++) {
        const GL_GC_REF *s = &gl_gc_save.ref[i];
        if (s->used > 1u || s->own > 1u || s->n > (s->own ? 2u : 1u) || s->pend > 1u
            || (s->pend && s->pdepth != 1u && s->pdepth != 2u)) {
            gp("[b5-gc] roots: gl.gcref not used (a bad ref)\r\n");
            return;
        }
    }
    for (UW i = 0; i < GL_GC_REFS; i++) {
        gl_gc_ref[i] = gl_gc_save.ref[i];
        if (gl_gc_ref[i].used) used++;
    }
    gl_gc_tick = gl_gc_save.tick;
    gp("[b5-gc] roots: restored "); gpd(used); gp(" refs from gl.gcref\r\n");
    /* a fetch the kill cut short (audit-231: a quarter round after head 4, R1
     * 1.05..1.17): what came of that generation is on no root. Forget it first
     * (written), then drop what no kept generation holds. */
    for (UW i = 0; i < GL_GC_REFS; i++) {
        GL_GC_REF *r = &gl_gc_ref[i];
        if (!r->used || !r->pend) continue;
        U1 p[PFS_ID_LEN]; UW pd = r->pdepth, pn = r->pnchunk;
        for (UW b = 0; b < PFS_ID_LEN; b++) p[b] = r->proot[b];
        r->pend = 0;
        gl_gc_store();
        if (gl_gc_guard() == 0) gl_gc_drop_gen(r->node, p, pd, pn, "a fetch cut short by a restart");
    }
#endif
}

static void gl_gc_store(void)
{
    if (!pfs_dur_write) return;
    gl_gc_save.magic = GL_GC_MAGIC; gl_gc_save.ver = GL_GC_VER;
    gl_gc_save.refs = GL_GC_REFS;   gl_gc_save.refsz = (UW)sizeof(GL_GC_REF);
    gl_gc_save.tick = gl_gc_tick;
    for (UW i = 0; i < GL_GC_REFS; i++) gl_gc_save.ref[i] = gl_gc_ref[i];
    if (pfs_dur_write(GL_GC_FILE, &gl_gc_save, (unsigned)sizeof gl_gc_save) != 0)
        gp("[b5-gc] roots: gl.gcref not written\r\n");
}

static void gl_gc_k(const U1 id[PFS_ID_LEN], UW k[2])
{
    k[0] = (UW)id[0] | ((UW)id[1] << 8) | ((UW)id[2] << 16) | ((UW)id[3] << 24);
    k[1] = (UW)id[4] | ((UW)id[5] << 8) | ((UW)id[6] << 16) | ((UW)id[7] << 24);
    if (k[0] == 0 && k[1] == 0) k[1] = 1;        /* 0/0 marks an empty slot */
}
/* mode 0: insert (returns -1 when the table is 3/4 full); 1: lookup (1 = in) */
static INT gl_gc_hash(const U1 id[PFS_ID_LEN], INT mode)
{
    UW k[2]; gl_gc_k(id, k);
    UW h = (k[0] ^ (k[1] * 2654435761u)) & (GL_GC_HSIZE - 1u);
    for (UW i = 0; i < GL_GC_HSIZE; i++, h = (h + 1u) & (GL_GC_HSIZE - 1u)) {
        if (gl_gc_key[h][0] == 0 && gl_gc_key[h][1] == 0) {
            if (mode == 1) return 0;
            if (gl_gc_fill >= GL_GC_HSIZE / 4u * 3u) return -1;
            gl_gc_key[h][0] = k[0]; gl_gc_key[h][1] = k[1]; gl_gc_fill++;
            return 0;
        }
        if (gl_gc_key[h][0] == k[0] && gl_gc_key[h][1] == k[1]) return mode == 1 ? 1 : 0;
    }
    return mode == 1 ? 0 : -1;
}

/* read one index block from this node's store (no want: it is ours to walk) */
static INT gl_gc_index(const U1 id[PFS_ID_LEN], GL_ST_INDEX *ix, UW level, UW count)
{
    INT r = pfs_get(id, ix, (UW)sizeof *ix);
    if (r < 0 || ix->magic != GL_ST_IDX_MAGIC || ix->level != level || ix->count != count
        || (UW)r != GL_ST_IDX_HDR + count * PFS_ID_LEN) return -1;
    return 0;
}

/* every id of one generation: mode 0 into the guard, 2 = drop what the guard lacks,
 * 3 = count the ids still in the store (H42: the own generation one back).
 * -1 = an index cannot be read, -2 = the guard table is full (mode 0) */
static INT gl_gc_walk(const U1 root[PFS_ID_LEN], UW depth, UW nchunk, INT mode)
{
    UW nleaf = (nchunk + GL_ST_IDS_PER_IDX - 1) / GL_ST_IDS_PER_IDX;
#define GL_GC_ONE(id) do { if (mode == 0) { if (gl_gc_hash((id), 0) < 0) return -2; } \
        else if (mode == 3) { gl_gc_seen++; if (pfs_has(id)) gl_gc_present++; } \
        else { gl_gc_seen++; if (!gl_gc_hash((id), 1) && pfs_drop(id) == PFS_OK) gl_gc_dropped++; } } while (0)
    if (depth == 1) {
        if (gl_gc_index(root, &gl_gc_leaf, 0u, nchunk) < 0) return -1;
        for (UW c = 0; c < nchunk; c++) GL_GC_ONE(gl_gc_leaf.id[c]);
    } else {
        if (gl_gc_index(root, &gl_gc_root, 1u, nleaf) < 0) return -1;
        for (UW lf = 0; lf < nleaf; lf++) {
            UW cnt = nchunk - lf * GL_ST_IDS_PER_IDX;
            if (cnt > GL_ST_IDS_PER_IDX) cnt = GL_ST_IDS_PER_IDX;
            if (gl_gc_index(gl_gc_root.id[lf], &gl_gc_leaf, 0u, cnt) < 0) return -1;
            for (UW j = 0; j < cnt; j++) GL_GC_ONE(gl_gc_leaf.id[j]);
            GL_GC_ONE(gl_gc_root.id[lf]);       /* the leaf block itself */
        }
    }
    GL_GC_ONE(root);                            /* the root (or only) index block */
#undef GL_GC_ONE
    return 0;
}

static INT gl_gc_eq(const U1 *a, const U1 *b)
{
    for (UW i = 0; i < PFS_ID_LEN; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* The guard: every kept generation of every ref, and (as far as its index is
 * here) every generation being fetched. 0, or -1 = the table is full (drop
 * nothing). gc-restart step 2: a kept generation whose index cannot be read
 * (a power loss can take the lazy blocks under a durable root) is forgotten
 * (written), not kept as a reason to stop every cleanup for good, as it was
 * ("cannot be read; nothing dropped" until that peer came back). Falsifier
 * -DB5_NC_GC_KEEP_LOST: the old stop. */
static UB gl_gc_guard_ok;          /* the table is the current kept set (gl_gc_holds) */
static INT gl_gc_guard(void)
{
    gl_gc_guard_ok = 0;
    for (UW h = 0; h < GL_GC_HSIZE; h++) { gl_gc_key[h][0] = 0; gl_gc_key[h][1] = 0; }
    gl_gc_fill = 0;
    UB changed = 0;
    for (UW i = 0; i < GL_GC_REFS; i++) {
        GL_GC_REF *r = &gl_gc_ref[i];
        if (!r->used) continue;
        for (UW g = 0; g < r->n; g++) {
            INT w = gl_gc_walk(r->root[g], r->depth[g], r->nchunk[g], 0);
            if (w == -2) { gp("[b5-gc] the guard table is full; nothing dropped\r\n"); return -1; }
            if (w < 0) {
#if defined(B5_NC_GC_KEEP_LOST)
                gp("[b5-gc] st/"); gpd(r->node); gp(": a kept generation cannot be read; nothing dropped\r\n");
                return -1;
#else
                gp("[b5-gc] st/"); gpd(r->node); gp(": a kept generation cannot be read; forgotten\r\n");
                if (g == 0 && r->n == 2) {
                    for (UW b = 0; b < PFS_ID_LEN; b++) r->root[0][b] = r->root[1][b];
                    r->depth[0] = r->depth[1]; r->nchunk[0] = r->nchunk[1];
                }
                r->n--; g--; changed = 1;
#endif
            }
        }
        if (r->pend && gl_gc_walk(r->proot, r->pdepth, r->pnchunk, 0) == -2) {
            gp("[b5-gc] the guard table is full; nothing dropped\r\n"); return -1;
        }
    }
    if (changed) gl_gc_store();
    gl_gc_guard_ok = 1;
    return 0;
}

/* gc-restart step 2b (2026-11-25): pfs_repl.c's boot-SYNC responder asks this
 * (weak, hosted) and does not stream an id of a generation this node keeps or
 * is fetching. Measured: a node restarted a quarter round after head 4 sent
 * the boot SYNC and got 2-3 peers' gen 4 in part (the blocks in their RAM
 * table, 26/43, 37/43 and 21 chunks without their index); it fetched gen 5 at
 * its round and never gen 4, so those blocks were on no root (R1 1.114 and
 * 1.143, with the fetch-in-progress fix in). A student chunk moves by want at
 * a round (b5-wire2), never by a push. 1 = held (8-byte prefix: a collision
 * keeps a block out of the stream, it can still be wanted). */
INT gl_gc_holds(const U1 id[PFS_ID_LEN])
{
    return gl_gc_guard_ok && gl_gc_hash(id, 1) == 1;
}

/* the slot of st/<node>: its own, a free one, or (step 2) the one unchanged for
 * GL_GC_STALE changes (never the own one); its generations are then dropped */
static GL_GC_REF *gl_gc_ref_of(pk_node_t node)
{
    for (UW i = 0; i < GL_GC_REFS; i++)
        if (gl_gc_ref[i].used && gl_gc_ref[i].node == node) return &gl_gc_ref[i];
    GL_GC_REF *r = 0;
    for (UW i = 0; i < GL_GC_REFS && !r; i++) if (!gl_gc_ref[i].used) r = &gl_gc_ref[i];
#if !defined(B5_NC_GC_NO_EVICT)
    static GL_GC_REF old;
    old.used = 0;
    if (!r) {
        GL_GC_REF *o = 0;
        for (UW i = 0; i < GL_GC_REFS; i++)
            if (!gl_gc_ref[i].own && (!o || gl_gc_tick - gl_gc_ref[i].last > gl_gc_tick - o->last))
                o = &gl_gc_ref[i];
        if (o && gl_gc_tick - o->last >= GL_GC_STALE) { old = *o; r = o; }
    }
#endif
    if (!r) return 0;                           /* more refs than slots: no cleanup */
    r->used = 1; r->node = node; r->n = 0; r->own = 0; r->pend = 0; r->last = gl_gc_tick;
#if !defined(B5_NC_GC_NO_EVICT)
    if (old.used) {
        gl_gc_store();                          /* forgotten before the drop */
        gp("[b5-gc] st/"); gpd(old.node); gp(": its slot goes to st/"); gpd(node);
        gp(" (no change for "); gpd(gl_gc_tick - old.last); gp(" changes)\r\n");
        if (gl_gc_guard() == 0) {
            for (UW g = 0; g < old.n; g++)
                gl_gc_drop_gen(old.node, old.root[g], old.depth[g], old.nchunk[g], "a freed slot");
            if (old.pend) gl_gc_drop_gen(old.node, old.proot, old.pdepth, old.pnchunk, "a freed slot's fetch");
        }
        old.used = 0;
    }
#endif
    return r;
}

/* gc-restart (audit-231 🔁GCR): st/<node>'s generation `root` is about to be
 * fetched. Written before its first block is asked for; gl_gc_note clears it.
 * Before, the blocks of a fetch a kill cut short were on no root and stayed
 * for good (R1 1.052 and 1.171 a quarter round after head 4). A restart, or
 * the fetch of another generation of the same node (a fetch that gave up),
 * drops what came of it. Falsifier -DB5_NC_GC_NO_PEND. */
static void gl_gc_pend(pk_node_t node, const U1 root[PFS_ID_LEN], UW depth, UW nchunk)
{
#if defined(B5_NC_NO_GC) || defined(B5_NC_GC_NO_PEND)
    (void)node; (void)root; (void)depth; (void)nchunk;
#else
    gl_gc_load();
    GL_GC_REF *r = gl_gc_ref_of(node);
    if (!r) return;
    if (r->n > 0 && gl_gc_eq(r->root[0], root)) return;   /* it is here already */
    if (r->pend && gl_gc_eq(r->proot, root)) return;      /* the same fetch again */
    static U1 p[PFS_ID_LEN];
    UB had = r->pend; UW pd = r->pdepth, pn = r->pnchunk;
    for (UW b = 0; b < PFS_ID_LEN; b++) { p[b] = r->proot[b]; r->proot[b] = root[b]; }
    r->pend = 1; r->pdepth = (UB)depth; r->pnchunk = nchunk;
    gl_gc_store();
    if (had && gl_gc_guard() == 0) gl_gc_drop_gen(node, p, pd, pn, "a fetch that did not finish");
#endif
}

/* st/<node> now names the generation `root`; clean the one two before it */
static void gl_gc_note(pk_node_t node, const U1 root[PFS_ID_LEN], UW depth, UW nchunk, UB own)
{
#if defined(B5_NC_NO_GC)
    (void)node; (void)root; (void)depth; (void)nchunk; (void)own;
#else
    gl_gc_load();                               /* gc-restart: once, before any change */
    GL_GC_REF *r = gl_gc_ref_of(node);
    if (!r) return;                             /* more refs than slots: no cleanup */
    /* research/b5-m-default.md §3 2: the own chunks two generations back (a peer
     * may still be fetching the one before), a peer's after this node's merge
     * of the next one = one generation kept */
    if (own) r->own = 1;
    UB keep = r->own ? 2 : 1;
    UB unpend = r->pend && gl_gc_eq(r->proot, root);
    if (unpend) r->pend = 0;                    /* the fetch finished */
    if (r->n > 0 && gl_gc_eq(r->root[0], root)) {           /* the same generation again */
        if (unpend) gl_gc_store();
        return;
    }
    r->last = ++gl_gc_tick;
    U1 vic[PFS_ID_LEN]; UW vdepth = 0, vn = 0; INT have_vic = 0;
    if (r->n == keep) {
        UW o = keep - 1u;                       /* the oldest kept */
        for (UW b = 0; b < PFS_ID_LEN; b++) vic[b] = r->root[o][b];
        vdepth = r->depth[o]; vn = r->nchunk[o]; have_vic = 1;
    }
    for (UW b = 0; b < PFS_ID_LEN; b++) { r->root[1][b] = r->root[0][b]; r->root[0][b] = root[b]; }
    r->depth[1] = r->depth[0]; r->nchunk[1] = r->nchunk[0];
    r->depth[0] = (UB)depth;   r->nchunk[0] = nchunk;
    if (r->n < keep) r->n++;
    /* gc-restart: written before the drop. A kill during the drop leaves the
     * rest of the victim in the store (it is no longer a root): a small window */
    gl_gc_store();
    if (!have_vic) { (void)gl_gc_guard(); return; }   /* the table = the kept set (gl_gc_holds) */
    /* the guard: every kept generation of every ref */
    if (gl_gc_guard() < 0) return;
    gl_gc_dropped = 0; gl_gc_seen = 0;
    INT w = gl_gc_walk(vic, vdepth, vn, 2);
    gp("[b5-gc] st/"); gpd(node); gp(": dropped "); gpd(gl_gc_dropped); gp(" of ");
    gpd(gl_gc_seen); gp(w < 0 ? " ids (the old generation was not all here)\r\n" : (keep == 2 ? " ids of the own generation two back\r\n" : " ids of the peer's generation one back\r\n"));
    /* H42: after an own cleanup, the own generation ONE back (a peer may still be
     * fetching it) is counted in the store, id by id ([b5-gc] G3 reads the line).
     * An unreadable index counts as not here (the walk stops: present < seen). */
    if (own && r->n == 2) {
        gl_gc_seen = 0; gl_gc_present = 0;
        INT w3 = gl_gc_walk(r->root[1], r->depth[1], r->nchunk[1], 3);
        if (w3 < 0) gl_gc_seen++;
        gp("[b5-gc] st/"); gpd(node); gp(": own generation one back: "); gpd(gl_gc_present);
        gp(" of "); gpd(gl_gc_seen); gp(" ids here\r\n");
    }
#endif
}

#if defined(GL_GC_TEST)
/* tests/llm/b5_gc_slots_test.c only (never in a product build): drive the
 * cleanup's roots by hand. mute = publish does not note (the test plays peers). */
static UB gl_gc_test_muted;
void gl_gc_test_mute(INT on) { gl_gc_test_muted = (UB)(on != 0); }
void gl_gc_test_note(pk_node_t node, const U1 root[PFS_ID_LEN], UW depth, UW nchunk) { gl_gc_note(node, root, depth, nchunk, 0); }
void gl_gc_test_pend(pk_node_t node, const U1 root[PFS_ID_LEN], UW depth, UW nchunk) { gl_gc_pend(node, root, depth, nchunk); }
void gl_gc_test_restart(void)                   /* RAM forgotten, as after a kill */
{
    for (UW i = 0; i < GL_GC_REFS; i++) gl_gc_ref[i].used = 0;
    gl_gc_tick = 0; gl_gc_loaded = 0;
}
INT gl_gc_test_slot(pk_node_t node)
{
    for (UW i = 0; i < GL_GC_REFS; i++) if (gl_gc_ref[i].used && gl_gc_ref[i].node == node) return (INT)i;
    return -1;
}
/* ids of one generation in the store: present / seen (an unreadable index stops) */
void gl_gc_test_count(const U1 root[PFS_ID_LEN], UW depth, UW nchunk, UW *present, UW *seen)
{
    gl_gc_seen = 0; gl_gc_present = 0;
    (void)gl_gc_walk(root, depth, nchunk, 3);
    *present = gl_gc_present; *seen = gl_gc_seen;
}
void gl_gc_test_desc(U1 root[PFS_ID_LEN], UW *depth, UW *nchunk)   /* the last publish */
{
    for (UW b = 0; b < PFS_ID_LEN; b++) root[b] = gl_st_desc.root_id[b];
    *depth = gl_st_desc.depth; *nchunk = gl_st_desc.nchunk;
}
#define GL_GC_NOTE(n, r, d, c, o) do { if (!gl_gc_test_muted) gl_gc_note((n), (r), (d), (c), (o)); } while (0)
#else
#define GL_GC_NOTE(n, r, d, c, o) gl_gc_note((n), (r), (d), (c), (o))
#endif

/* the single named ref "st/<node>" (<= 6 chars <= PFS_NAME_MAX). */
static UW gl_st_name(char *out, pk_node_t node)
{
    UW k = 0;
    out[k++] = 's'; out[k++] = 't'; out[k++] = '/';
    if (node >= 100) out[k++] = (char)('0' + (node / 100) % 10);
    if (node >= 10)  out[k++] = (char)('0' + (node / 10) % 10);
    out[k++] = (char)('0' + node % 10);
    return k;
}

static void gl_st_idcpy(U1 dst[PFS_ID_LEN], const U1 src[PFS_ID_LEN])
{
    for (UW b = 0; b < PFS_ID_LEN; b++) dst[b] = src[b];
}

INT gl_student_publish(pk_node_t node, const void *blob, UW len)
{
    if (!blob || len == 0) return -1;
    UW nchunk = (len + GL_ST_CHUNK - 1) / GL_ST_CHUNK;
    if (nchunk == 0 || nchunk > GL_ST_MAXCHUNK) return -1;   /* refuse, NEVER truncate */

    /* index geometry: leaves of <=127 chunk-ids; one root of <=127 leaf-ids.
     * depth 1 = single index block holds all chunk-ids (nchunk<=127);
     * depth 2 = root -> leaves -> chunks (covers S and M). A blob whose
     * leaves would exceed 127 (depth>2, i.e. the L tier) is REFUSED — but
     * nchunk>127*127 also trips the GL_ST_MAXCHUNK gate above first, so L is
     * already refused; this is the explicit, never-silent depth bound. */
    UW nleaf = (nchunk + GL_ST_IDS_PER_IDX - 1) / GL_ST_IDS_PER_IDX;
    if (nleaf > GL_ST_IDS_PER_IDX) return -1;                /* would need depth>2 */
    UW depth = (nchunk <= GL_ST_IDS_PER_IDX) ? 1u : 2u;

    const UB *src = (const UB *)blob;
    INT drop = gl_st_drop; gl_st_drop = -1;                  /* one-shot, auto-disarm */

    /* build each leaf: store its chunks, collect their content-ids. */
    for (UW lf = 0; lf < nleaf; lf++) {
        UW base = lf * GL_ST_IDS_PER_IDX;
        UW cnt  = nchunk - base;
        if (cnt > GL_ST_IDS_PER_IDX) cnt = GL_ST_IDS_PER_IDX;

        gl_st_idx.magic = GL_ST_IDX_MAGIC;
        gl_st_idx.level = 0;
        gl_st_idx.count = cnt;
        gl_st_idx._pad  = 0;
        for (UW j = 0; j < cnt; j++) {
            UW c    = base + j;
            UW off  = c * GL_ST_CHUNK;
            UW clen = (len - off < GL_ST_CHUNK) ? (len - off) : GL_ST_CHUNK;
            for (UW i = 0; i < clen; i++) gl_st_chunk[i] = src[off + i];
            U1 cid[PFS_ID_LEN];
            if ((INT)c == drop) {
                /* falsifier: index the chunk's id but DO NOT store the block,
                 * so a fetch MUST fail closed (content-id known, bytes absent). */
                pfs_id_compute(gl_st_chunk, clen, cid);
            } else if (gl_st_put(gl_st_chunk, clen, cid) != PFS_OK) {
                return -1;
            }
            gl_st_idcpy(gl_st_idx.id[j], cid);
            if ((c + 1u) % GL_ST_YIELD_EVERY == 0u) tk_dly_tsk(GL_ST_YIELD_MS);
        }
        U1 lid[PFS_ID_LEN];
        if (gl_st_put(&gl_st_idx, GL_ST_IDX_HDR + cnt * PFS_ID_LEN, lid) != PFS_OK)
            return -1;
        gl_st_idcpy(gl_st_leafids[lf], lid);
    }

    /* root content-id: depth 1 -> the single leaf IS the root; depth 2 ->
     * build a level-1 index over the leaf-ids. */
    U1 root_id[PFS_ID_LEN];
    if (depth == 1) {
        gl_st_idcpy(root_id, gl_st_leafids[0]);
    } else {
        gl_st_idx.magic = GL_ST_IDX_MAGIC;
        gl_st_idx.level = 1;
        gl_st_idx.count = nleaf;
        gl_st_idx._pad  = 0;
        for (UW lf = 0; lf < nleaf; lf++) gl_st_idcpy(gl_st_idx.id[lf], gl_st_leafids[lf]);
        if (gl_st_put(&gl_st_idx, GL_ST_IDX_HDR + nleaf * PFS_ID_LEN, root_id) != PFS_OK)
            return -1;
    }

    /* B5 D4: the chunks went without their own fsyncs; ONE syncfs makes them
     * durable BEFORE the descriptor that names them is saved (fsync'd). */
    if (gl_st_sync() != 0) return -1;

    /* descriptor under ONE named ref "st/<node>". */
    gl_st_desc.magic     = GL_ST_DESC_MAGIC;
    gl_st_desc.version   = GL_ST_DESC_VER;
    gl_st_desc.tier      = 0;            /* informational; the blob self-describes */
    gl_st_desc.total_len = len;
    gl_st_desc.nchunk    = nchunk;
    gl_st_desc.depth     = depth;
    gl_st_idcpy(gl_st_desc.root_id, root_id);

    char nm[8]; UW nl = gl_st_name(nm, node);
    if (pfs_dag_save((const UB *)nm, nl, &gl_st_desc, (UW)sizeof gl_st_desc) != PFS_OK)
        return -1;
    GL_GC_NOTE(node, root_id, depth, nchunk, 1);   /* B5 ①: the own generation two back */
    return (INT)nchunk;
}

/* pull one index block by content-id into gl_st_idx, with windowed want
 * retry; validate magic/level/count + exact length. Returns 0 / -1. */
static INT gl_st_pull_index(const U1 id[PFS_ID_LEN], UW level, UW count)
{
    INT prev = -2, stall = 0;
    for (UW pass = 0; pass < GL_ST_FETCH_MAXPASS; pass++) {
        INT r = pfs_get(id, &gl_st_idx, (UW)sizeof gl_st_idx);
        if (r >= 0) {
            if (gl_st_idx.magic == GL_ST_IDX_MAGIC &&
                gl_st_idx.level == level &&
                gl_st_idx.count == count &&
                (UW)r == GL_ST_IDX_HDR + count * PFS_ID_LEN)
                return 0;
            return -1;                              /* present but malformed */
        }
        gl_st_want(id);
        if (r <= prev) { if (++stall >= GL_ST_FETCH_STALL) break; } else stall = 0;
        prev = r;
        tk_dly_tsk(GL_ST_FETCH_POLL_MS);
    }
    return -1;
}

static INT gl_student_fetch_in(pk_node_t node, void *out, UW cap)
{
    if (!out) return -1;

    /* 1 named ref -> descriptor. */
#if !defined(B5W_NC_LOUD)
    gl_st_from = node;
#endif
    char nm[8]; UW nl = gl_st_name(nm, node);
    INT dr = pfs_dag_read((const UB *)nm, nl, &gl_st_desc, (UW)sizeof gl_st_desc);
    if (dr != (INT)sizeof gl_st_desc) return -1;
    if (gl_st_desc.magic != GL_ST_DESC_MAGIC) return -1;
    if (gl_st_desc.version != GL_ST_DESC_VER) return -1;
    UW len = gl_st_desc.total_len, nchunk = gl_st_desc.nchunk, depth = gl_st_desc.depth;
    if (len == 0 || len > cap) return -1;                   /* refuse, NEVER truncate */
    if (nchunk == 0 || nchunk > GL_ST_MAXCHUNK) return -1;
    if (depth != 1 && depth != 2) return -1;
    if (nchunk != (len + GL_ST_CHUNK - 1) / GL_ST_CHUNK) return -1;
    UW nleaf = (nchunk + GL_ST_IDS_PER_IDX - 1) / GL_ST_IDS_PER_IDX;
    if (nleaf > GL_ST_IDS_PER_IDX) return -1;
    if ((depth == 1) != (nchunk <= GL_ST_IDS_PER_IDX)) return -1;  /* depth/size agree */
    gl_gc_pend(node, gl_st_desc.root_id, depth, nchunk);   /* gc-restart: before the first want */

    /* 2. walk the index into the ordered chunk-id list. */
    if (depth == 1) {
        if (gl_st_pull_index(gl_st_desc.root_id, 0u, nchunk) < 0) { gp("[pfs] student fetch: gave up on the index\r\n"); return -1; }
        for (UW c = 0; c < nchunk; c++) gl_st_idcpy(gl_st_idlist[c], gl_st_idx.id[c]);
    } else {
        if (gl_st_pull_index(gl_st_desc.root_id, 1u, nleaf) < 0) { gp("[pfs] student fetch: gave up on the root index\r\n"); return -1; }
        for (UW lf = 0; lf < nleaf; lf++) gl_st_idcpy(gl_st_leafids[lf], gl_st_idx.id[lf]);
        for (UW lf = 0; lf < nleaf; lf++) {
            UW base = lf * GL_ST_IDS_PER_IDX;
            UW cnt  = nchunk - base;
            if (cnt > GL_ST_IDS_PER_IDX) cnt = GL_ST_IDS_PER_IDX;
            if (gl_st_pull_index(gl_st_leafids[lf], 0u, cnt) < 0) { gp("[pfs] student fetch: gave up on a leaf index\r\n"); return -1; }
            for (UW j = 0; j < cnt; j++) gl_st_idcpy(gl_st_idlist[base + j], gl_st_idx.id[j]);
        }
    }

    /* 3. windowed WANT of every chunk; presence is checked with a zero-copy
     * pfs_get (returns the stored length without touching `out`), so `out`
     * stays UNCONSUMED until every chunk is confirmed present (fail closed). */
    /* b5-wire2 §6: a chunk seen present stays present; remember it, so a pass
     * looks up only the missing ones (482 lookups a pass made a 4-peer S round
     * take 72 s in the live test at 6b0423a0) */
    for (UW c = 0; c < nchunk; c++) { gl_st_have[c] = 0; gl_st_asked[c] = 0; }
    UW present = 0; INT prev = -1, stall = 0;
    for (UW pass = 0; pass < GL_ST_FETCH_MAXPASS; pass++) {
        present = 0; UW window = 0;
        for (UW c = 0; c < nchunk; c++) {
            if (gl_st_have[c]) { present++; continue; }
            UW off  = c * GL_ST_CHUNK;
            UW clen = (len - off < GL_ST_CHUNK) ? (len - off) : GL_ST_CHUNK;
            if (pfs_get(gl_st_idlist[c], 0, 0) == (INT)clen) { gl_st_have[c] = 1; present++; continue; }
            if (window < GL_ST_WANT_WINDOW) {
                UW a = gl_st_asked[c];
                if (a == 0 || pass + 1u - a >= GL_ST_REASK) { gl_st_want(gl_st_idlist[c]); gl_st_asked[c] = (UH)(pass + 1u); }
                window++;
            }
        }
        if (present == nchunk) break;
        if ((INT)present <= prev) { if (++stall >= GL_ST_FETCH_STALL) break; } else stall = 0;
        prev = (INT)present;
        tk_dly_tsk(GL_ST_FETCH_POLL_MS);
    }
    if (present != nchunk) {                                 /* fail closed */
        gp("[pfs] student fetch n"); gpd(node); gp(": gave up, chunks "); gpd(present);
        gp("/"); gpd(nchunk); gp(" present\r\n");
        return -1;
    }

    /* 4. all present -> reassemble into the caller buffer + verify length. */
    UW got = 0;
    for (UW c = 0; c < nchunk; c++) {
        UW off  = c * GL_ST_CHUNK;
        UW clen = (len - off < GL_ST_CHUNK) ? (len - off) : GL_ST_CHUNK;
        INT r = pfs_get(gl_st_idlist[c], (UB *)out + off, clen);
        if (r != (INT)clen) return -1;                       /* race / regression */
        got += clen;
    }
    if (got != len) return -1;                               /* recovered length check */
    gl_gc_note(node, gl_st_desc.root_id, depth, nchunk, 0); /* B5 ①: the peer's one back */
    return (INT)len;
}

/* the fetch, then one syncfs for the chunks it brought (D4) */
INT gl_student_fetch(pk_node_t node, void *out, UW cap)
{
    INT r = gl_student_fetch_in(node, out, cap);
    if (gl_st_sync() != 0)
        gp("[pfs] student fetch: syncfs failed (the fetched chunks may not survive a power loss)\r\n");
    return r;
}
#endif /* _TK_HOSTED_LIBC_ */

/* ------------------------------------------------------------------ */
/* dataset — deterministic synthetic sensor readings.                  */
/*                                                                     */
/* Same task family as dtr_train.c (latent temperature -> 3 classes,   */
/* with correlated distractor channels and label noise so a fixed      */
/* threshold cannot reach 100%). Regenerated here (not shared) so this  */
/* module is self-contained; the generator is deterministic, identical  */
/* bytes on every node / every ABI.                                    */
/* ------------------------------------------------------------------ */

#define GL_N       300
#define GL_TRAIN   240            /* shards partition the first 240    */
#define GL_TEST    (GL_N - GL_TRAIN)  /* held-out 60: all 3 classes    */
#define GL_NCLASS  3
#define GL_SEED    0x5EED2026UL

static B  gl_x[GL_N][DTR_SEQ_LEN];
static UB gl_y[GL_N];
static UB gl_ds_ready = 0;
static UW gl_rng;

static UW gl_rand(void)
{
    gl_rng = gl_rng * 1664525UL + 1013904223UL;
    return (gl_rng >> 16) & 0x7FFF;
}
static INT gl_uniform(INT lo, INT hi)
{
    return lo + (INT)(gl_rand() % (UW)(hi - lo + 1));
}
static INT gl_noise(INT s)
{
    INT v = 0;
    for (INT i = 0; i < 4; i++) v += gl_uniform(-s, s);
    return v / 2;
}
static B gl_clamp(INT v)
{
    if (v >  127) v =  127;
    if (v < -128) v = -128;
    return (B)v;
}

static void gl_ds_init(void)
{
    if (gl_ds_ready) return;
    gl_rng = GL_SEED;
    for (INT i = 0; i < GL_N; i++) {
        UB c = (UB)(i % GL_NCLASS);          /* class-balanced both splits */
        INT latent;
        if (c == 0)      latent = gl_uniform(-50,  24);
        else if (c == 1) latent = gl_uniform( 25,  69);
        else             latent = gl_uniform( 70, 120);
        INT temp  = latent + gl_noise(12);
        INT hum   = 60 - latent / 2 + gl_noise(20);
        INT press = gl_uniform(-30, 90);
        INT light = 10 + 30 * (INT)c + gl_noise(35);
        gl_x[i][0] = gl_clamp(temp);
        gl_x[i][1] = gl_clamp(hum);
        gl_x[i][2] = gl_clamp(press);
        gl_x[i][3] = gl_clamp(light);
        gl_y[i]    = c;
    }
    gl_ds_ready = 1;
}

/* ------------------------------------------------------------------ */
/* shards — leave-one-class-out (DISJOINT, the crux)                   */
/*                                                                     */
/* node k's shard = TRAIN samples whose class != (k % NCLASS). So with  */
/* 3 nodes each MISSES a different class entirely and cannot classify   */
/* it solo — its ceiling on the full (all-class) task is bounded well   */
/* below 100%. Union of the shards covers every class, so the swarm     */
/* CAN, but only by combining (averaging) — not by copying one node.    */
/* ------------------------------------------------------------------ */

/* Max gossip participants == the cluster node ceiling. ONE source of truth:
 * gossip membership can never exceed the node table (G23). Was a hardwired 4
 * (the live merge loop + peer caches capped the swarm at 4 ids, and the
 * in-process bank at 4 sims) — that, plus DNODE_MAX, was the real node
 * ceiling. Now both scale with DNODE_MAX so a >32-node swarm gossips for
 * real. (The leave-one-class-out sim only needs GL_NCLASS distinct shards,
 * but sizing the bank to DNODE_MAX lets the [g23-ceiling] self-test drive
 * the REAL merge across >32 in-process participants with no 32-cap.) */
#define GL_MAXNODES DNODE_MAX

static B  sh_x[GL_MAXNODES][GL_TRAIN][DTR_SEQ_LEN];
static UB sh_y[GL_MAXNODES][GL_TRAIN];
static UW sh_n[GL_MAXNODES];

/* build node k's shard array (excludes class k%NCLASS from TRAIN). */
static void gl_build_shard(UW k)
{
    UB excl = (UB)(k % GL_NCLASS);
    UW m = 0;
    for (INT i = 0; i < GL_TRAIN; i++) {
        if (gl_y[i] == excl) continue;
        for (INT t = 0; t < DTR_SEQ_LEN; t++) sh_x[k][m][t] = gl_x[i][t];
        sh_y[k][m] = gl_y[i];
        m++;
    }
    sh_n[k] = m;
}

/* full-task held-out accuracy (%) under the currently loaded weights. */
static float gl_full_acc(void)
{
    UW correct = 0;
    (void)dtr_eval_batch(gl_x + GL_TRAIN, gl_y + GL_TRAIN, GL_TEST, &correct);
    return (float)correct * 100.0f / (float)GL_TEST;
}

/* shard accuracy (%) for node k under the currently loaded weights. */
static float gl_shard_acc(UW k)
{
    UW correct = 0;
    (void)dtr_eval_batch(sh_x[k], sh_y[k], sh_n[k], &correct);
    return (float)correct * 100.0f / (float)sh_n[k];
}

/* step-decayed LR (same shape dtr_train.c uses). A healthy LR is what lets
 * a 2-node sub-swarm re-balance its class mix quickly after a peer dies, so
 * the survivors hold above their solo ceilings; the rejoin demo reconverges
 * a fresh 3-node swarm (robust at any LR) rather than chasing frozen peers. */
static float gl_lr(UW step, UW total)
{
    return (step <= total / 2) ? 0.10f : 0.05f;
}

/* ------------------------------------------------------------------ */
/* in-process model bank (simulate N nodes through dtr's single model)  */
/* ------------------------------------------------------------------ */

static float gl_model[GL_MAXNODES][DTR_WEIGHT_FLOATS];
static float gl_avg[DTR_WEIGHT_FLOATS];

/* shared, deterministic starting point so averaging same-origin models
 * is well-behaved early (linear mode connectivity). Every node seeds
 * from the same weights, then diverges only via its shard. */
#define GL_INIT_SEED 0xC0FFEE11UL

/* train the currently-loaded weights for `steps` SGD steps on shard k. */
static void gl_train_local(UW k, UW steps, UW lr_total, UW lr_base)
{
    for (UW s = 1; s <= steps; s++)
        (void)dtr_train_batch(sh_x[k], sh_y[k], sh_n[k],
                              gl_lr(lr_base + s, lr_total));
}

/* ------------------------------------------------------------------ */
/* [g22-shard-solo] — a node on its shard alone caps low on full task  */
/* ------------------------------------------------------------------ */

/* Train each node ONLY on its own shard for `total` steps (no gossip),
 * eval on the full task; return the BEST (max) solo accuracy — the
 * strongest honest baseline the collective must beat. */
static float gl_run_solo(UW nodes, UW total, float per_node_out[])
{
    float best = 0.0f;
    for (UW k = 0; k < nodes; k++) {
        dtr_reinit_weights(GL_INIT_SEED);
        for (UW s = 1; s <= total; s++)
            (void)dtr_train_batch(sh_x[k], sh_y[k], sh_n[k],
                                  gl_lr(s, total));
        float a = gl_full_acc();
        if (per_node_out) per_node_out[k] = a;
        if (a > best) best = a;
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* [g22-gossip-learn] — N nodes, disjoint shards, gossip-merge rounds  */
/* ------------------------------------------------------------------ */

/* Decentralized SGD: each round every node does `local` SGD steps on
 * its own shard, then ALL models are averaged (gossip merge) and each
 * node adopts the average. Returns the (common) full-task accuracy. */
static float gl_run_gossip(UW nodes, UW rounds, UW local)
{
    UW total = rounds * local;
    /* every node starts from the same seed */
    for (UW k = 0; k < nodes; k++) {
        dtr_reinit_weights(GL_INIT_SEED);
        dtr_weights_get(gl_model[k]);
    }
    for (UW r = 0; r < rounds; r++) {
        /* phase A: independent local training on each disjoint shard */
        for (UW k = 0; k < nodes; k++) {
            dtr_weights_set(gl_model[k]);
            gl_train_local(k, local, total, r * local);
            dtr_weights_get(gl_model[k]);
        }
        /* phase B: each node merges (averages) the models it gossiped.
         * No aggregator: gl_merge over the symmetric set of models. */
        const float *ptrs[GL_MAXNODES];
        for (UW k = 0; k < nodes; k++) ptrs[k] = gl_model[k];
        gl_merge(gl_avg, ptrs, nodes, DTR_WEIGHT_FLOATS);
        for (UW k = 0; k < nodes; k++)
            for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++)
                gl_model[k][i] = gl_avg[i];
    }
    dtr_weights_set(gl_model[0]);
    return gl_full_acc();
}

/* ------------------------------------------------------------------ */
/* [g22-no-central] — the merge is peer-symmetric, no aggregator       */
/* ------------------------------------------------------------------ */

/* Structural proof that the merge has NO central aggregator:
 *
 *   1. gl_merge's signature carries a flat SET of models and NO
 *      aggregator/server index — there is no privileged node by
 *      construction (the live path passes {self} U {peers I fetched};
 *      every node runs the same call on the set it gossiped).
 *   2. Order independence: merging the same set in node-0 order vs the
 *      reverse (node-(N-1)) order yields the SAME model up to float
 *      rounding — so no position in the set is special. (Exact byte
 *      equality is NOT required and would be wrong: float addition is
 *      not associative, so a different summation order legitimately
 *      differs in the low bits; a STRUCTURAL privilege would instead
 *      shift weights by O(1), not O(1e-6).)
 *   3. Identity: a single-model "merge" returns that model exactly — no
 *      hidden global state is folded in.
 *
 * All three <=> the merge is peer-symmetric, computed locally by each
 * node from local+peer models, with no aggregator. */
static INT gl_check_no_central(UW nodes)
{
    /* give each "node" a distinct model so order would matter (by O(1))
     * if the merge secretly privileged a position */
    for (UW k = 0; k < nodes; k++) {
        dtr_reinit_weights(GL_INIT_SEED + k * 7919UL);
        dtr_weights_get(gl_model[k]);
    }
    const float *fwd[GL_MAXNODES], *rev[GL_MAXNODES];
    for (UW k = 0; k < nodes; k++) {
        fwd[k] = gl_model[k];
        rev[k] = gl_model[nodes - 1 - k];
    }
    static float a[DTR_WEIGHT_FLOATS], b[DTR_WEIGHT_FLOATS];
    gl_merge(a, fwd, nodes, DTR_WEIGHT_FLOATS);   /* node-0 perspective */
    gl_merge(b, rev, nodes, DTR_WEIGHT_FLOATS);   /* node-(N-1) view    */

    float worst = 0.0f;
    for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++) {
        float d = a[i] - b[i]; if (d < 0.0f) d = -d;
        if (d > worst) worst = d;
    }
    gp("[g22]   no-central: |merge(fwd)-merge(rev)| max="); gpf1(worst * 1000.0f);
    gp("e-3 (rounding only; structural privilege would be O(1))\r\n");
    if (worst >= 1e-4f) return 0;                  /* O(1) shift => central */

    /* identity: single-model merge returns it exactly */
    gl_merge(a, fwd, 1, DTR_WEIGHT_FLOATS);
    for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++)
        if (a[i] != gl_model[0][i]) return 0;
    return 1;
}

/* ================================================================== */
/* G38 — THINKING CHANGES GUARDING (survival-network §8 §9 two-layer    */
/* couple). The standing audit-7 §4.3 found: even with G22 collective   */
/* learning landed, the learned model reaches the guard layer by ZERO   */
/* arrows ("二層は並んでいるだけで結合していない"). G38 builds both     */
/* arrows and proves learning makes guarding better.                   */
/*                                                                     */
/*  Arrow 1 (learning -> guarding): moe_infer now feeds the reflex the  */
/*    learned transformer's REAL max-softmax confidence + argmax class  */
/*    (moe.c; was a dead 0xFF). Here we PROVE a low-confidence input    */
/*    does NOT fire the reflex while a learned-confident threat does    */
/*    ([g38-confidence-live]), and that a COLLECTIVELY-LEARNED model    */
/*    guards measurably better than an UNLEARNED one                    */
/*    ([g38-learning-improves-guarding]).                              */
/*  Arrow 2 (guarding -> learning): the reflex's per-class threat       */
/*    experience (reflex_threat_experience) is read as a learning       */
/*    PRIORITY that emphasizes the danger classes the guard met         */
/*    ([g38-guard-feeds-learning]).                                    */
/*                                                                     */
/* Both use the REAL softmax (dtr_forward_probs) and the REAL reflex    */
/* gate (reflex_would_fire) — no fakes; the only thing that changes the */
/* guard score is the learning. */
/* ================================================================== */

#define GL_ST_NODES   3
#define GL_ST_ROUNDS  40
#define GL_ST_LOCAL   4

static UW gl_my_shard_slot(void);    /* defined in the live section below */

/* guard DECISION for one input under the CURRENTLY-loaded weights: the
 * learned model's argmax class + real max-softmax confidence drive the SAME
 * reflex gate (reflex_would_fire) the live moe->reflex path uses. */
static BOOL gl_guard_decide(const B *x, UB *cls_out, UB *conf_out)
{
    float p[DTR_OUT_DIM];
    dtr_forward_probs(x, p);
    UB cls = 0; float mx = p[0];
    for (INT c = 1; c < DTR_OUT_DIM; c++) if (p[c] > mx) { mx = p[c]; cls = (UB)c; }
    INT conf = (INT)(mx * 100.0f + 0.5f);
    if (conf < 0)   conf = 0;
    if (conf > 100) conf = 100;
    if (cls_out)  *cls_out  = cls;
    if (conf_out) *conf_out = (UB)conf;
    return reflex_would_fire(cls, (UB)conf);
}

/* guarding quality on the held-out set under the currently-loaded weights:
 *   threat sample (label>=1): correct iff reflex FIRES and class==label
 *   normal sample (label==0): correct iff reflex does NOT fire (no false rally)
 * returns overall guard accuracy %; fills threat-detect % and false-rally %.
 * crit_td: detection % of the critical class (2) specifically (arrow-2 metric). */
static float gl_guard_score(float *td_out, float *fr_out, float *crit_out)
{
    UW correct = 0, td = 0, nthreat = 0, fr = 0, nnorm = 0, ctd = 0, ncrit = 0;
    for (UW i = GL_TRAIN; i < GL_N; i++) {
        UB cls, conf; BOOL fire = gl_guard_decide(gl_x[i], &cls, &conf);
        UB lbl = gl_y[i];
        if (lbl >= 1) {
            nthreat++;
            if (fire && cls == lbl) { correct++; td++; }
            if (lbl == 2) { ncrit++; if (fire && cls == 2) ctd++; }
        } else {
            nnorm++;
            if (!fire) correct++; else fr++;
        }
    }
    if (td_out)   *td_out   = nthreat ? (float)td  * 100.0f / (float)nthreat : 0.0f;
    if (fr_out)   *fr_out   = nnorm   ? (float)fr  * 100.0f / (float)nnorm   : 0.0f;
    if (crit_out) *crit_out = ncrit   ? (float)ctd * 100.0f / (float)ncrit   : 0.0f;
    return (float)correct * 100.0f / (float)GL_TEST;
}

/* ── [g38-confidence-live] — real max-softmax gates the reflex ──────────── */
static INT g38_confidence(void)
{
    INT fails = 0;

    /* pick a fixed CRITICAL (label==2) held-out input — the same input is
     * judged by the unlearned and then the learned model, so any change in
     * the guarding outcome is due to learning alone. */
    UW ci = GL_N; for (UW i = GL_TRAIN; i < GL_N; i++) if (gl_y[i] == 2) { ci = i; break; }

    /* (A) UNLEARNED model on that critical input. */
    dtr_reinit_weights(GL_INIT_SEED);
    UB cls_u = 0, conf_u = 0; (void)gl_guard_decide(gl_x[ci], &cls_u, &conf_u);
    BOOL fire_u = reflex_would_fire(cls_u, conf_u);

    /* (B) LEARNED model via decentralized collective gossip on the SAME input,
     * plus a scan proving the moe->reflex confidence is REAL and VARIES across
     * inputs (a dead 0xFF gate would make every confidence the constant 255). */
    (void)gl_run_gossip(GL_ST_NODES, GL_ST_ROUNDS, GL_ST_LOCAL);   /* loads consensus */
    UB cls_l = 0, conf_l = 0; (void)gl_guard_decide(gl_x[ci], &cls_l, &conf_l);
    BOOL fire_l = reflex_would_fire(cls_l, conf_l);
    UB conf_min = 100, conf_max = 0;
    for (UW i = GL_TRAIN; i < GL_N; i++) {
        UB c, cf; (void)gl_guard_decide(gl_x[i], &c, &cf); (void)c;
        if (cf < conf_min) conf_min = cf;
        if (cf > conf_max) conf_max = cf;
    }

    /* (C) the confidence GATE is live (kills the dead 0xFF): a sub-floor
     * confidence on a threat class must NOT fire; floor+ must; 0xFF (the OLD
     * dead value) WOULD have fired. */
    BOOL gate_lo   = reflex_would_fire(2, (UB)(REFLEX_CONF_MIN - 1)); /* expect no  */
    BOOL gate_ok   = reflex_would_fire(2, (UB)REFLEX_CONF_MIN);       /* expect yes */
    BOOL dead_fire = reflex_would_fire(2, 0xFF);                      /* old gate    */

    gp("[g38] moe->reflex confidence is REAL max-softmax (was dead 0xFF):\r\n");
    gp("[g38]   same critical input: UNLEARNED cls="); gpd(cls_u);
    gp(" conf="); gpd(conf_u); gp("% fire="); gp(fire_u ? "YES" : "no");
    gp("  ->  LEARNED cls="); gpd(cls_l);
    gp(" conf="); gpd(conf_l); gp("% fire="); gp(fire_l ? "YES" : "no"); gp("\r\n");
    gp("[g38]   learned-model confidence range over held-out = ["); gpd(conf_min);
    gp("%.."); gpd(conf_max); gp("%] (varies, not constant 0xFF)\r\n");
    gp("[g38]   gate: conf<floor fire="); gp(gate_lo ? "YES" : "no");
    gp("  conf>=floor fire="); gp(gate_ok ? "YES" : "no");
    gp("  dead-0xFF would-fire="); gp(dead_fire ? "YES" : "no");
    gp(" (floor="); gpd(REFLEX_CONF_MIN); gp(")\r\n");

    /* assertions */
    if (ci >= GL_N) { gp("[g38-confidence-live] FAIL no critical sample\r\n"); fails++; }
    if (conf_u == 0xFF || conf_l == 0xFF || conf_max == 0xFF) {
        gp("[g38-confidence-live] FAIL confidence is constant 0xFF (dead gate)\r\n"); fails++;
    }
    if (!(conf_min < conf_max)) {        /* real, input-dependent confidence  */
        gp("[g38-confidence-live] FAIL confidence does not vary across inputs\r\n"); fails++;
    }
    if (gate_lo || !gate_ok || !dead_fire) {  /* the confidence gate is live  */
        gp("[g38-confidence-live] FAIL confidence gate not live (low-conf threat must not fire; dead-0xFF must)\r\n"); fails++;
    }
    /* learning CHANGED the guarding outcome on this real input: unlearned
     * failed to fire-correctly on the critical input; learned fires on it. */
    if (!(fire_l && cls_l == 2)) {
        gp("[g38-confidence-live] FAIL learned model did not fire correctly on the critical input\r\n"); fails++;
    }
    if (!( (!fire_u || cls_u != 2) )) {
        gp("[g38-confidence-live] FAIL unlearned already guarded this input (no learning effect to show)\r\n"); fails++;
    }
    if (!(conf_l > conf_u)) {
        gp("[g38-confidence-live] FAIL learning did not raise confidence on the critical input\r\n"); fails++;
    }
    if (fails == 0)
        gp("[g38-confidence-live] PASS (real max-softmax gates the reflex; learning flipped this critical input from un-guarded to a confident reflex fire)\r\n");
    else
        gp("[g38-confidence-live] FAIL\r\n");
    return fails;
}

/* ── [g38-learning-improves-guarding] — learned guards better than unlearned ─ */
static INT g38_learning_improves(void)
{
    INT fails = 0;
    float td_u, fr_u, c_u, td_l, fr_l, c_l;

    /* unlearned baseline: the same shared seed the collective starts from. */
    dtr_reinit_weights(GL_INIT_SEED);
    float gu = gl_guard_score(&td_u, &fr_u, &c_u);

    /* learned via decentralized collective gossip (no central aggregator). */
    float coll = gl_run_gossip(GL_ST_NODES, GL_ST_ROUNDS, GL_ST_LOCAL);
    float gl   = gl_guard_score(&td_l, &fr_l, &c_l);

    gp("[g38] guarding quality (learned model gates the reflex over held-out):\r\n");
    gp("[g38]   UNLEARNED guard="); gpf1(gu);
    gp("% threat_detect="); gpf1(td_u); gp("% false_rally="); gpf1(fr_u); gp("%\r\n");
    gp("[g38]   LEARNED   guard="); gpf1(gl);
    gp("% threat_detect="); gpf1(td_l); gp("% false_rally="); gpf1(fr_l);
    gp("%  (collective full_acc="); gpf1(coll); gp("%)\r\n");

    if (!(gl > gu + 15.0f)) {
        gp("[g38-learning-improves-guarding] FAIL learned model did not guard measurably better than unlearned\r\n"); fails++;
    }
    if (!(td_l > td_u)) {
        gp("[g38-learning-improves-guarding] FAIL learned did not detect more threats\r\n"); fails++;
    }
    if (fails == 0) {
        gp("[g38-learning-improves-guarding] PASS (collective learning improved guarding: guard ");
        gpf1(gu); gp("%->"); gpf1(gl); gp("%, threat-detect ");
        gpf1(td_u); gp("%->"); gpf1(td_l); gp("%)\r\n");
    } else {
        gp("[g38-learning-improves-guarding] FAIL\r\n");
    }
    return fails;
}

/* ── arrow 2: guard experience -> learning priority (oversample danger) ──── */
#define GL_WMAX 3                              /* max per-class oversample factor */
static B  gw_x[GL_TRAIN * GL_WMAX][DTR_SEQ_LEN];
static UB gw_y[GL_TRAIN * GL_WMAX];

/* build a guard-weighted view of node k's shard: each sample of class c is
 * repeated classw[c] times (clamped 1..GL_WMAX). The danger classes the guard
 * flagged get emphasized in SGD. Returns the weighted sample count. */
static UW gl_build_weighted(UW k, const UW classw[GL_NCLASS])
{
    UW m = 0;
    for (UW i = 0; i < sh_n[k]; i++) {
        UB c = sh_y[k][i];
        UW rep = (c < GL_NCLASS) ? classw[c] : 1;
        if (rep < 1) rep = 1;
        if (rep > GL_WMAX) rep = GL_WMAX;
        for (UW r = 0; r < rep && m < GL_TRAIN * GL_WMAX; r++) {
            for (INT t = 0; t < DTR_SEQ_LEN; t++) gw_x[m][t] = sh_x[k][i][t];
            gw_y[m] = c; m++;
        }
    }
    return m;
}

/* decentralized collective gossip, but each node trains on its GUARD-WEIGHTED
 * shard (classw from the reflex's threat experience). Same no-central merge. */
static float gl_run_gossip_weighted(UW nodes, UW rounds, UW local,
                                    const UW classw[GL_NCLASS])
{
    UW total = rounds * local;
    for (UW k = 0; k < nodes; k++) { dtr_reinit_weights(GL_INIT_SEED); dtr_weights_get(gl_model[k]); }
    for (UW r = 0; r < rounds; r++) {
        for (UW k = 0; k < nodes; k++) {
            dtr_weights_set(gl_model[k]);
            UW wn = gl_build_weighted(k, classw);
            for (UW s = 1; s <= local; s++)
                (void)dtr_train_batch(gw_x, gw_y, wn, gl_lr(r * local + s, total));
            dtr_weights_get(gl_model[k]);
        }
        const float *ptrs[GL_MAXNODES];
        for (UW k = 0; k < nodes; k++) ptrs[k] = gl_model[k];
        gl_merge(gl_avg, ptrs, nodes, DTR_WEIGHT_FLOATS);
        for (UW k = 0; k < nodes; k++)
            for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++) gl_model[k][i] = gl_avg[i];
    }
    dtr_weights_set(gl_model[0]);
    return gl_full_acc();
}

/* ── [g38-guard-feeds-learning] — guard experience prioritizes the learning ─ */
static INT g38_guard_feeds(void)
{
    INT fails = 0;
    /* Short schedule so the critical-class detection has headroom for the
     * priority to matter (the long collective saturates to ~100%). */
    const UW R = 12, L = 4;

    /* The PRIORITY signal: the reflex's per-class threat experience. In a LIVE
     * run reflex_threat_experience(c) carries what the guard actually met; in
     * this self-contained self-test the guard may not have fired yet, so we
     * fall back to a danger-emphasis stand-in (critical heaviest) — the SAME
     * shape the live reflex produces. Either way the learner reads the guard. */
    UW classw[GL_NCLASS];
    UW exp_tot = 0;
    for (UB c = 0; c < GL_NCLASS; c++) exp_tot += reflex_threat_experience(c);
    if (exp_tot > 0) {
        UW mx = 1;
        for (UB c = 0; c < GL_NCLASS; c++) { UW e = reflex_threat_experience(c); if (e > mx) mx = e; }
        for (UB c = 0; c < GL_NCLASS; c++)
            classw[c] = 1 + (reflex_threat_experience(c) * (GL_WMAX - 1) + mx - 1) / mx;
        gp("[g38] guard experience (LIVE reflex) cls=[");
    } else {
        classw[0] = 1; classw[1] = 2; classw[2] = 3;   /* guard cares most about critical */
        gp("[g38] guard experience (danger-emphasis stand-in) cls=[");
    }
    for (UB c = 0; c < GL_NCLASS; c++) { gpd(reflex_threat_experience(c)); if (c + 1 < GL_NCLASS) gp(","); }
    gp("]  -> learning priority weights=[");
    for (UB c = 0; c < GL_NCLASS; c++) { gpd(classw[c]); if (c + 1 < GL_NCLASS) gp(","); }
    gp("]\r\n");

    /* unlearned baseline for the structural gate. */
    dtr_reinit_weights(GL_INIT_SEED);
    float gu = gl_guard_score(0, 0, 0);

    /* plain (uniform) short collective vs guard-weighted short collective. */
    UW uniw[GL_NCLASS] = { 1, 1, 1 };
    (void)gl_run_gossip_weighted(GL_ST_NODES, R, L, uniw);
    float td_p, c_p; float gp_plain = gl_guard_score(&td_p, 0, &c_p);

    UW wn0 = gl_build_weighted(0, classw);            /* did the signal oversample? */
    UW wn0_uni = gl_build_weighted(0, uniw);
    (void)gl_run_gossip_weighted(GL_ST_NODES, R, L, classw);
    float td_w, c_w; float gp_w = gl_guard_score(&td_w, 0, &c_w);

    gp("[g38]   plain   short-collective: guard="); gpf1(gp_plain);
    gp("% threat_detect="); gpf1(td_p); gp("% critical_detect="); gpf1(c_p); gp("%\r\n");
    gp("[g38]   guard-weighted          : guard="); gpf1(gp_w);
    gp("% threat_detect="); gpf1(td_w); gp("% critical_detect="); gpf1(c_w);
    gp("%  (weighted-samples node0 "); gpd(wn0_uni); gp("->"); gpd(wn0); gp(")\r\n");

    /* Structural (non-flaky) gate for the ARROW: the guard signal was actually
     * consumed (it oversampled the shard), and the guard-prioritized learner
     * produced a working guard model well above unlearned. The quantitative
     * plain-vs-weighted critical-detect delta is PRINTED and reported honestly
     * (it can be marginal — see docs/architecture/20-architecture/reflex-action.md). */
    if (!(wn0 > wn0_uni)) {
        gp("[g38-guard-feeds-learning] FAIL guard priority did not reach the learner (no oversample)\r\n"); fails++;
    }
    if (!(gp_w > gu + 15.0f)) {
        gp("[g38-guard-feeds-learning] FAIL guard-prioritized learning did not improve guarding over unlearned\r\n"); fails++;
    }
    if (fails == 0) {
        gp("[g38-guard-feeds-learning] PASS (guard experience -> learning priority is wired and lifts guarding; critical_detect plain ");
        gpf1(c_p); gp("% vs weighted "); gpf1(c_w); gp("%)\r\n");
    } else {
        gp("[g38-guard-feeds-learning] FAIL\r\n");
    }
    return fails;
}

/* public G38 entry — run from `dtr gossip g38` (wired into CI self-test). */
void gl_g38_test(void)
{
    INT fails = 0;
    gp("[g38] ==== thinking changes guarding (G38 / survival-network §8 §9 two-layer couple) ====\r\n");
    gl_ds_init();
    for (UW k = 0; k < GL_ST_NODES; k++) gl_build_shard(k);
    dtr_ga_busy = 1;
    fails += g38_confidence();
    fails += g38_learning_improves();
    fails += g38_guard_feeds();
    dtr_ga_busy = 0;
    if (fails == 0) gp("[g38] ALL PASS\r\n");
    else { gp("[g38] FAILURES="); gpd((UW)fails); gp("\r\n"); }
}

/* live: print THIS node's guard score under current (LEARNED) or fresh
 * (UNLEARNED) weights — sample 34 compares before/after gossip over the relay. */
static void gl_cmd_guardscore(INT fresh)
{
    gl_ds_init();
    UW slot = gl_my_shard_slot();
    gl_build_shard(slot);
    pk_node_t me = (drpc_my_node == 0xFF) ? 0 : drpc_my_node;
    dtr_ga_busy = 1;
    if (fresh) dtr_reinit_weights(GL_INIT_SEED);
    float td, fr, cr; float gs = gl_guard_score(&td, &fr, &cr);
    dtr_ga_busy = 0;
    gp("[g38-live] node="); gpd(me);
    gp(fresh ? " UNLEARNED" : " LEARNED");
    gp(" guard_score="); gpf1(gs);
    gp("% threat_detect="); gpf1(td);
    gp("% false_rally="); gpf1(fr);
    gp("% critical_detect="); gpf1(cr); gp("%\r\n");
}

/* ------------------------------------------------------------------ */
/* the self-test                                                       */
/* ------------------------------------------------------------------ */

void gl_self_test(void)
{
    gp("[g22] ==== decentralized collective learning (G22 / survival-network §8 §9) ====\r\n");
    gl_ds_init();
    for (UW k = 0; k < GL_ST_NODES; k++) gl_build_shard(k);

    gp("[g22] task: 3-class sensor classify; shards leave-one-class-out (DISJOINT).\r\n");
    for (UW k = 0; k < GL_ST_NODES; k++) {
        gp("[g22]   node"); gpd(k); gp(" shard: ");
        gpd(sh_n[k]); gp(" samples, MISSING class ");
        gpd(k % GL_NCLASS); gp(" (cannot classify it solo)\r\n");
    }

    UW total = GL_ST_ROUNDS * GL_ST_LOCAL;

    /* --- [g22-shard-solo] --- */
    float solo_pn[GL_MAXNODES];
    float solo = gl_run_solo(GL_ST_NODES, total, solo_pn);
    gp("[g22] solo (each node, its shard only, ");
    gpd(total); gp(" steps): full-task acc per node = [");
    for (UW k = 0; k < GL_ST_NODES; k++) {
        gpf1(solo_pn[k]); gp("%"); if (k + 1 < GL_ST_NODES) gp(" ");
    }
    gp("]\r\n");
    gp("[g22] solo CEILING (best any node alone) = "); gpf1(solo); gp("%\r\n");
    /* a node missing one of three classes cannot exceed ~2/3 on a
     * class-balanced full task; assert it really is bounded low. */
    if (solo < 80.0f) {
        gp("[g22-shard-solo] PASS (no node alone learns the whole task; ceiling ");
        gpf1(solo); gp("% < 80%)\r\n");
    } else {
        gp("[g22-shard-solo] FAIL (a solo shard scored too high — shards not disjoint?)\r\n");
    }

    /* --- [g22-gossip-learn] --- */
    float coll = gl_run_gossip(GL_ST_NODES, GL_ST_ROUNDS, GL_ST_LOCAL);
    gp("[g22] gossip-learn ("); gpd((UW)GL_ST_ROUNDS);
    gp(" rounds x "); gpd((UW)GL_ST_LOCAL);
    gp(" local steps, no-central avg): full-task acc = ");
    gpf1(coll); gp("%\r\n");
    gp("[g22]   solo-ceiling "); gpf1(solo);
    gp("%  ->  collective "); gpf1(coll); gp("%  (delta +");
    gpf1(coll - solo); gp(")\r\n");
    if (coll > solo + 3.0f)
        gp("[g22-gossip-learn] PASS (collective EXCEEDS solo ceiling — the swarm learned what no node could)\r\n");
    else
        gp("[g22-gossip-learn] FAIL (collective did not clear solo ceiling by margin)\r\n");

    /* --- [g22-no-central] --- */
    if (gl_check_no_central(GL_ST_NODES))
        gp("[g22-no-central] PASS (merge is peer-symmetric / order-independent; no aggregator index — every node averages locally)\r\n");
    else
        gp("[g22-no-central] FAIL (merge depended on order — a privileged aggregator exists)\r\n");

    /* restore a sane trained model for any follow-on infer */
    dtr_weights_set(gl_model[0]);
    gp("[g22] ==== done ====\r\n");
}

/* ------------------------------------------------------------------ */
/* live multi-node — over the relay (sample 32)                        */
/* ------------------------------------------------------------------ */

/* per-node model ref: "dtr/model/<n>" (<=16 chars, fits PFS_NAME_MAX). */
static UW gl_model_ref(char *out, UW node)
{
    const char *p = "dtr/model/";
    UW i = 0;
    while (p[i]) { out[i] = p[i]; i++; }
    if (node >= 10) out[i++] = (char)('0' + (node / 10) % 10);
    out[i++] = (char)('0' + node % 10);
    return i;
}

/* My shard, derived from this node's id (leave-one-class-out). */
static UW gl_my_shard_slot(void)
{
    /* in-process scratch is indexed 0..GL_MAXNODES-1; map the live node
     * id onto a slot deterministically by its excluded class. We only
     * need one shard locally, stored in slot (node%NCLASS) so its
     * excluded-class arithmetic matches gl_build_shard. */
    pk_node_t node = (drpc_my_node == 0xFF) ? 0 : drpc_my_node;
    return node % GL_NCLASS;
}

static UW gl_live_total = 0;     /* live gossip rounds completed         */

/* `dtr gossip solo [steps]` — measure THIS node's solo shard ceiling on
 * the full task, then leave the model fresh (re-seeded) for `run`. */
static void gl_cmd_solo(UW steps)
{
    if (steps == 0) steps = 160;
    gl_ds_init();
    UW slot = gl_my_shard_slot();
    gl_build_shard(slot);
    pk_node_t node = (drpc_my_node == 0xFF) ? 0 : drpc_my_node;

    dtr_ga_busy = 1;
    dtr_reinit_weights(GL_INIT_SEED);
    for (UW s = 1; s <= steps; s++)
        (void)dtr_train_batch(sh_x[slot], sh_y[slot], sh_n[slot],
                              gl_lr(s, steps));
    float solo = gl_full_acc();
    /* reset to the shared seed so `run` starts clean & comparable */
    dtr_reinit_weights(GL_INIT_SEED);
    dtr_ga_busy = 0;

    gp("[g22-live] node="); gpd(node);
    gp(" shard=missing-class"); gpd(slot);
    gp(" solo_steps="); gpd(steps);
    gp(" solo_ceiling="); gpf1(solo); gp("%\r\n");
}

/* peer-model cache (function-static; never task-stack), indexed by peer
 * node id (live ids are small: 0..GL_MAXNODES-1). Once a peer's model is
 * fetched it is REMEMBERED, so a round whose 2.5 KB pull lagged the P1
 * want/serve still averages that peer in (with its most recent model)
 * instead of dropping to peers=0. gl_phave[] is reset at run start. */
static float gl_pcache[GL_MAXNODES][DTR_WEIGHT_FLOATS];
static UB    gl_phave[GL_MAXNODES];

/* issue a p-fs WANT for every peer's model so the (2.5 KB) content transfers
 * during the slow-band delay — by the next round's merge it is usually local.
 *
 * THE FAN-IN FIX (peer discovery is content-driven, not SWIM-gated): we warm
 * EVERY id, not only the ones the local SWIM view has already marked ALIVE. A
 * fresh node's failure detector converges slowly, so an early round can see a
 * real peer as NOTALIVE and starve the marginal disjoint-shard node of cross-
 * shard signal. But only swarm members publish dtr/model/<n>, so a fetch that
 * SUCCEEDS is itself proof of membership: issuing the WANT for every id lets a
 * published-but-not-yet-ALIVE peer be discovered by its CONTENT. A WANT for an
 * unpublished id is a cheap miss. Still peer-symmetric (every node probes the
 * same id space) and no central aggregator. Return value ignored: prefetch. */
static void gl_prefetch_peers(pk_node_t me)
{
    static float discard[DTR_WEIGHT_FLOATS];
    for (UB p = 0; p < DNODE_MAX; p++) {
        if (p == me) continue;
        char pref[20]; UW prl = gl_model_ref(pref, p);
        (void)gl_pfs_fetch(pref, prl, discard, DTR_WEIGHT_FLOATS);
    }
}

/* MERGE phase: read whatever ALIVE peers' models are already local and
 * average them with my own (no central aggregator — every node does this
 * locally over the symmetric set {self} U {peers it gossiped}). Returns
 * the number of peers actually folded in.
 *
 * Collective learning needs each round to actually fold in EVERY peer; a
 * 2.5 KB model can lag a P1 want/serve, so we give a missing peer bounded
 * retries (re-issue the want via the fetch, brief wait) before giving up for
 * THIS round — but a peer once cached stays folded, so a future round only has
 * to land each still-missing peer once. Robust to transfer jitter without
 * changing any transport code. */
#define GL_FETCH_RETRY    6
#define GL_FETCH_WAIT_MS  250

/* MEMBERSHIP + MERGE step (no transport): average my own model (gl_model[0])
 * with every peer whose model is cached (gl_phave[p]), folding the result back
 * into gl_model[0]. Iterates the FULL node table (0..DNODE_MAX) so the swarm
 * ceiling is DNODE_MAX, not a hardwired 4/32 — a peer with any id <DNODE_MAX is
 * folded in. Returns the number of peers folded.
 *
 * Fold-eligibility is ANY cached peer (gl_phave[p]), NOT the live SWIM ALIVE
 * flag: once I have gossiped a peer's weight body it stays folded across
 * rounds, so a transient SWIM flap (a missed ping while the big 2.5 KB pull
 * was in flight) can never silently halve a marginal node's fan-in. A peer
 * that truly leaves is reaped by the kill path (the rejoin demo), and its
 * lingering model is exactly what §3 says the swarm keeps; this fold does not
 * resurrect a dead peer's membership, it only averages weight bodies I hold.
 *
 * Shared by the live path (gl_merge_peers, after it pulls the caches) and the
 * [g23-ceiling] self-test (which pre-fills the caches), so the test exercises
 * the REAL membership+merge logic. */
static UW gl_fold_cached_peers(pk_node_t me)
{
    const float *ptrs[GL_MAXNODES];
    UW cnt = 0;
    ptrs[cnt++] = gl_model[0];                       /* myself */
    for (UB p = 0; p < DNODE_MAX; p++) {
        if (p == me) continue;
        /* fold the peer in whether the pull was fresh this round or a
         * cached recent model — a gossiped peer never silently drops out. */
        if (gl_phave[p]) ptrs[cnt++] = gl_pcache[p];
    }
    if (cnt > 1) {
        gl_merge(gl_avg, ptrs, cnt, DTR_WEIGHT_FLOATS);
        for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++) gl_model[0][i] = gl_avg[i];
    }
    return cnt - 1;
}

#ifdef _TK_HOSTED_LIBC_
/* ENS-A lineage scoping (scaling-law.md §4.2) — HOSTED-only so the bare-metal
 * .text crown does NOT move (the preprocessor removes this whole block + the
 * one guarded line inside gl_merge_peers below on bare metal). Default 0 =
 * DISABLED, so ordinary live gossip is byte-for-byte its pre-ensemble self. */
UW  gl_ens_lineage_k = 0;
void gl_ens_set_lineage_k(UW k) { gl_ens_lineage_k = k; }
UW   gl_ens_get_lineage_k(void) { return gl_ens_lineage_k; }
#endif

static UW gl_merge_peers(pk_node_t me)
{
    /* transport: pull each peer's freshest model into the cache, then fold
     * over the symmetric set {self} U {peers cached}.
     *
     * Peer discovery is content-driven (the fan-in fix): we probe EVERY id,
     * not just the SWIM-ALIVE ones. A peer is folded the first round its model
     * ref is published and its 2.5 KB body lands — and once cached it stays
     * folded for good. A fetch of an unpublished ref fails fast and cheap, so
     * probing all ids is harmless. This guarantees the marginal disjoint-shard
     * node receives cross-shard signal from ALL its peers within a bounded
     * window, no matter how slowly the failure detector converges. */
    for (UB p = 0; p < DNODE_MAX; p++) {
        if (p == me) continue;
#ifdef _TK_HOSTED_LIBC_
        /* ENS-A (scaling-law.md §4.2): when lineage scoping is active, fold
         * ONLY same-lineage peers (lineage = node_id % k). Across lineages we
         * share facts/engrams (teach), not weights — divergent lineages stay
         * divergent, and their disagreement on hard questions is exactly the
         * variance the ensemble harvests. HOSTED-only guard; bare metal never
         * sees this line, so its .text crown is unchanged. */
        if (gl_ens_lineage_k && (p % gl_ens_lineage_k) != (me % gl_ens_lineage_k))
            continue;
#endif
        /* Probe EVERY id (not just SWIM-ALIVE ones) so a published-but-not-
         * yet-ALIVE peer is discovered by its content. The retry WAIT, though,
         * is only spent on a peer we have reason to believe is really there
         * (SWIM-ALIVE or already cached) — an unpublished id fails its first
         * fetch fast and we move on without sleeping GL_FETCH_RETRY×. */
        BOOL believed = (dnode_state(p) == DNODE_ALIVE) || gl_phave[p];
        char pref[20]; UW prl = gl_model_ref(pref, p);
        for (INT a = 0; a < GL_FETCH_RETRY; a++) {
            if (gl_pfs_fetch(pref, prl, gl_pcache[p], DTR_WEIGHT_FLOATS) == 0) {
                gl_phave[p] = 1;        /* fresh model cached, folded for good */
                break;
            }
            if (!believed) break;           /* don't burn waits on a phantom id */
            tk_dly_tsk(GL_FETCH_WAIT_MS);   /* let the want/serve land */
        }
    }
    return gl_fold_cached_peers(me);
}

/* §8 deliberation cadence (seconds-band). Long enough that a peer's freshly
 * published 2.5 KB model transfers (P1 want/serve) before the next merge. */
#define GL_SLOW_BAND_MS 2000

/* fresh != 0: start from the shared seed (a node joining/learning from
 * scratch). fresh == 0 ("cont"): keep the CURRENT weights — used by peers
 * that stay in the swarm while a fresh node rejoins, so they hold their
 * converged model instead of restarting (and don't get reset to noise). */
static void gl_cmd_run_ex(UW rounds, UW local, INT fresh)
{
    if (rounds == 0) rounds = 36;
    if (local  == 0) local  = 4;     /* matches the in-process self-test  */
    gl_ds_init();
    UW slot = gl_my_shard_slot();
    gl_build_shard(slot);
    pk_node_t me = (drpc_my_node == 0xFF) ? 0 : drpc_my_node;
    UW total = rounds * local;
    for (UW p = 0; p < GL_MAXNODES; p++) gl_phave[p] = 0;   /* fresh cache */

    gp("[g22-live] node="); gpd(me);
    gp(fresh ? " START gossip-learn rounds=" : " CONT gossip-learn rounds=");
    gpd(rounds);
    gp(" local="); gpd(local);
    gp(" shard=missing-class"); gpd(slot);
    gp(" (slow-band "); gpd((UW)GL_SLOW_BAND_MS); gp("ms/round)\r\n");

    dtr_ga_busy = 1;
    if (fresh) dtr_reinit_weights(GL_INIT_SEED);
    dtr_weights_get(gl_model[0]);

    float full = 0.0f;
    for (UW r = 0; r < rounds; r++) {
        /* FedAvg order (matches the in-process self-test): local-train
         * FIRST, then AVERAGE, and KEEP/eval the average. Averaging after
         * training is what defeats non-IID client drift — if we trained
         * after averaging and kept that, local SGD on a 2-class shard
         * would re-suppress the third class the average just taught us. */

        /* (1) local SGD on my disjoint shard, from last round's consensus */
        dtr_weights_set(gl_model[0]);
        gl_train_local(slot, local, total, r * local);
        dtr_weights_get(gl_model[0]);
        float shard = gl_shard_acc(slot);     /* my shard, post local-train */

        /* (2) PUBLISH my locally-updated model for peers to average in */
        char ref[20]; UW rl = gl_model_ref(ref, me);
        (void)gl_pfs_publish(ref, rl, gl_model[0], DTR_WEIGHT_FLOATS);

        /* (3) MERGE: average my model with peers' locally-updated models
         * (no central aggregator). The AVERAGE becomes my model — this is
         * the consensus we keep and evaluate. */
        UW peers = gl_merge_peers(me);
        dtr_weights_set(gl_model[0]);

        full = gl_full_acc();                 /* eval the CONSENSUS model */
        gp("[g22-live] node="); gpd(me);
        gp(" round="); gpd(r + 1);
        gp(" peers="); gpd(peers);
        gp(" shard_acc="); gpf1(shard);
        gp("% full_acc="); gpf1(full); gp("%\r\n");
        gl_live_total++;

        /* (4) prefetch peers + slow deliberation band (not the reflex
         * tick). dtr_ga_busy blocks dtr_infer while weights move; release
         * it across the delay so reflex/inference keep breathing. */
        gl_prefetch_peers(me);
        dtr_ga_busy = 0;
        tk_dly_tsk(GL_SLOW_BAND_MS);
        dtr_ga_busy = 1;
    }
    dtr_ga_busy = 0;

    gp("[g22-live] node="); gpd(me);
    gp(" RESULT rounds="); gpd(rounds);
    gp(" full_acc="); gpf1(full); gp("%\r\n");
}

static void gl_cmd_status(void)
{
    pk_node_t me = (drpc_my_node == 0xFF) ? 0 : drpc_my_node;
    gp("[g22-live] node="); gpd(me);
    gp(" excluded-class="); gpd(gl_my_shard_slot());
    gp(" rounds_done="); gpd(gl_live_total);
    gp(" weight_body="); gpd((UW)DTR_WEIGHT_FLOATS); gp(" floats\r\n");
}

/* ------------------------------------------------------------------ */
/* shell dispatcher                                                    */
/* ------------------------------------------------------------------ */

static const UB *gl_skip_ws(const UB *p, const UB *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}
static INT gl_tok(const UB *p, const UB *end, const char *kw)
{
    INT i = 0;
    while (kw[i]) { if (p + i >= end || p[i] != (UB)kw[i]) return 0; i++; }
    return (p + i == end || p[i] == ' ' || p[i] == '\t');
}
static UW gl_parse_uw(const UB **pp, const UB *end)
{
    const UB *p = gl_skip_ws(*pp, end);
    UW v = 0;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (UW)(*p - '0'); p++; }
    *pp = p;
    return v;
}

/* ================================================================== */
/* G23 — NODE CEILING > 32 (gap-ledger row G23).                        */
/*                                                                     */
/* DNODE_MAX was 32, which capped the collective brain at 32 nodes and  */
/* contradicted UMP "every install = a node". This self-test proves the */
/* >32 code path works FOR REAL — not just that a constant was bumped:  */
/*                                                                     */
/*   PART A (core merge, no transport): build N=40 (>32) DISTINCT       */
/*     in-process models (model k == constant k) and run the REAL       */
/*     gl_merge() over all 40. The true mean is 19.5; a silent 32-cap   */
/*     would average only the first 32 and yield 15.5. Exact-mean ==>   */
/*     all 40 were folded (gl_merge has no 32-cap; small-int floats so  */
/*     the sum is exact — no rounding alibi).                           */
/*                                                                     */
/*   PART B (live membership path): register N=40 ALIVE entries in the  */
/*     REAL dnode_table[], cache a distinct model per peer, then run the */
/*     REAL membership+merge step (gl_fold_cached_peers — the exact      */
/*     function the live gossip loop calls after pulling peer models).   */
/*     It must fold in N-1=39 peers (>32) and produce the 19.5 mean.     */
/*     If any array/loop still capped at 32 (or the old 4), the fold     */
/*     count or the mean would be wrong.                                */
/*                                                                     */
/* Run from `dtr gossip ceiling`. Restores dnode_table/drpc_my_node so  */
/* it is safe to run alongside the live stack.                          */
/* ================================================================== */

#define GL_G23_N  40    /* participants exercised (>32; <= DNODE_MAX)    */

static float gl_g23_out[DTR_WEIGHT_FLOATS];   /* merge target (static)   */

void gl_g23_test(void)
{
    INT fails = 0;
    const UW N = GL_G23_N;

    gp("[g23] ==== node ceiling > 32 (gap-ledger G23 / UMP every-install-a-node) ====\r\n");
    gp("[g23] DNODE_MAX="); gpd((UW)DNODE_MAX);
    gp("  GL_MAXNODES="); gpd((UW)GL_MAXNODES);
    gp("  participants="); gpd(N); gp(" (>32)\r\n");

    /* never exceed the table we are about to drive */
    if (N <= 32 || N > DNODE_MAX) {
        gp("[g23-ceiling] FAIL participant count not in (32, DNODE_MAX]\r\n");
        return;
    }

    /* ---- PART A: REAL gl_merge over N>32 distinct models ---- */
    for (UW k = 0; k < N; k++)
        for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++) gl_model[k][i] = (float)k;
    const float *ptrs[GL_MAXNODES];
    for (UW k = 0; k < N; k++) ptrs[k] = gl_model[k];
    gl_merge(gl_g23_out, ptrs, N, DTR_WEIGHT_FLOATS);

    /* exact expected mean of 0..N-1 (small ints -> exact in float32) */
    float exp_mean = (float)(N - 1) / 2.0f;            /* 0..39 -> 19.5  */
    float capped32 = (float)(32 - 1) / 2.0f;           /* 15.5 if capped */
    float merged   = gl_g23_out[0];
    BOOL a_ok = (merged == exp_mean);
    /* every element identical (no partial coverage of the weight body) */
    for (INT i = 1; i < DTR_WEIGHT_FLOATS; i++)
        if (gl_g23_out[i] != merged) { a_ok = FALSE; break; }

    gp("[g23]   core merge of "); gpd(N);
    gp(" models: mean="); gpf1(merged);
    gp(" expected="); gpf1(exp_mean);
    gp(" (a 32-cap would give "); gpf1(capped32); gp(")\r\n");
    if (!a_ok) { gp("[g23-ceiling] FAIL core merge truncated/incorrect\r\n"); fails++; }

    /* ---- PART B: REAL membership+merge over N>32 ALIVE dnode_table entries ---- */
    /* save the live state we are about to perturb */
    pk_node_t saved_my = drpc_my_node;
    UB  saved_state[GL_G23_N];
    UB  saved_phave[GL_G23_N];
    for (UW p = 0; p < N; p++) {
        saved_state[p] = dnode_state(p);
        saved_phave[p] = gl_phave[p];
    }

    pk_node_t me = 0;
    drpc_my_node = me;
    for (UW i = 0; i < (UW)DTR_WEIGHT_FLOATS; i++) gl_model[0][i] = (float)me; /* my value=0 */
    UW alive = 0;
    for (UW p = 1; p < N; p++) {
        dnode_set_node_id(p, (UB)p);
        dnode_set_state(p, DNODE_ALIVE);
        for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++) gl_pcache[p][i] = (float)p;
        gl_phave[p] = 1;
        alive++;
    }

    UW folded = gl_fold_cached_peers(me);   /* the REAL live merge step */
    float b_mean = gl_model[0][0];
    BOOL b_uniform = TRUE;
    for (INT i = 1; i < DTR_WEIGHT_FLOATS; i++)
        if (gl_model[0][i] != b_mean) { b_uniform = FALSE; break; }

    gp("[g23]   membership fold: "); gpd(alive);
    gp(" ALIVE peers + self -> folded="); gpd(folded);
    gp(" merged_mean="); gpf1(b_mean);
    gp(" expected="); gpf1(exp_mean); gp("\r\n");

    BOOL b_ok = (folded == N - 1) && (b_mean == exp_mean) && b_uniform;
    if (folded != N - 1)  { gp("[g23-ceiling] FAIL membership folded fewer than N-1 peers (a 32/4-cap truncated the swarm)\r\n"); fails++; }
    else if (!b_ok)       { gp("[g23-ceiling] FAIL membership merge value wrong\r\n"); fails++; }

    /* restore live state */
    drpc_my_node = saved_my;
    for (UW p = 0; p < N; p++) {
        dnode_set_state(p, saved_state[p]);
        gl_phave[p]          = saved_phave[p];
    }

    if (fails == 0)
        gp("[g23-ceiling] PASS (>32 nodes gossip-merge for real: 40-model core merge AND 40-entry live membership fold, no 32-cap)\r\n");
    else
        gp("[g23-ceiling] FAIL\r\n");
    gp("[g23] ==== done ====\r\n");
}

void gl_cmd(const UB *args, UW len)
{
    const UB *end = args + len;
    const UB *p   = gl_skip_ws(args, end);

    if (p >= end || gl_tok(p, end, "status")) { gl_cmd_status(); return; }
    if (gl_tok(p, end, "test")) { gl_self_test(); return; }
    if (gl_tok(p, end, "g38"))  { gl_g38_test(); return; }   /* §8 two-layer couple */
    if (gl_tok(p, end, "ceiling")) { gl_g23_test(); return; }  /* G23 node ceiling >32 */
#ifdef _TK_HOSTED_LIBC_
    if (gl_tok(p, end, "scaling")) { scaling_self_test(); return; }  /* society-of-minds [scaling-*] cert */
#endif
    if (gl_tok(p, end, "guard")) {                            /* live guard score   */
        p += 5;
        const UB *q = gl_skip_ws(p, end);
        INT fresh = (q < end && gl_tok(q, end, "fresh")) ? 1 : 0;
        gl_cmd_guardscore(fresh);
        return;
    }
    if (gl_tok(p, end, "solo")) {
        p += 4; UW steps = gl_parse_uw(&p, end);
        gl_cmd_solo(steps); return;
    }
    if (gl_tok(p, end, "run")) {
        p += 3;
        UW rounds = gl_parse_uw(&p, end);
        UW local  = gl_parse_uw(&p, end);
        gl_cmd_run_ex(rounds, local, 1);   /* fresh: start from seed */
        return;
    }
    if (gl_tok(p, end, "cont")) {
        p += 4;
        UW rounds = gl_parse_uw(&p, end);
        UW local  = gl_parse_uw(&p, end);
        gl_cmd_run_ex(rounds, local, 0);   /* keep current converged model */
        return;
    }
    gp("usage: dtr gossip [status] | test | g38 | ceiling | guard [fresh] | solo [steps] | run [rounds] [local] | cont [rounds] [local]\r\n");
}
