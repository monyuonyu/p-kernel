/*
 *  arch/common/ark_rec.c — the ark's records and their index (ark-entry.md 2.1, 2.4, 2.7;
 *  lines E1, E2, E4 of 3). Hosted only (v1 keeps records in the ARK backend, 2.2).
 *
 *  A record is a chain of versions. Each version is one ARK_REC block (no name, through
 *  pfs_repl_put) that points at its previous version by id and at its body fragments
 *  (ARK_FRAG blocks of up to 3584 bytes; their first 4 bytes are not MW_MAGIC, so mw_gc
 *  never picks them). Nothing is overwritten: a new version is new blocks.
 *
 *  `ark/rec` holds only the index: a descriptor {nrec, depth, fanout, root id}. The
 *  index is a tree of ARK_IDX blocks, `fanout` ids each (127 fill a 4 KB block); level 0
 *  holds each record's newest ARK_REC id. When the tree is full the old root becomes
 *  child 0 of a new root and depth grows by one, so a record keeps its place. Write
 *  order (2.1): fragments -> ARK_REC -> index leaf .. root -> the ark/rec version. A
 *  stop anywhere before the last step leaves the previous index whole (line E1c).
 *  Limit: every change leaves `depth` old index blocks (ark_compact keeps all blocks).
 *
 *  `ark/heir` holds the heir's public key (33 B: 1 set / 0 cleared, key). Its versions
 *  are pfs_dag's chain (the log reads back 64 versions at most, PFSD_LOG_MAX).
 *
 *  E3 (ark-entry.md 5): every version is also sealed (a new key box per version, the
 *  record key wrapped to the node's ark key in PKERNEL_PFS_DIR/ark.key) as AKSF fragments
 *  and an AKSR, and a sealed twin of the index (AKSI nodes, AKSD descriptor; same fanout,
 *  at most 124) is written with it. Only these "AKS" blocks leave the node; `ark rec
 *  restore <AKSD id>` on a node with the same ark.key rebuilds everything from them.
 *  Negative controls: -DARK_REC_PLAIN, -DARK_REC_SAME_KEY, -DARK_REC_INDEX_LOCAL,
 *  -DARK_REC_AAD_INNER (tests/host/run_ark_e3.sh). Test env: PKERNEL_ARK_REC_TAMPER=1.
 *
 *  The plain ark stays on its node: pfs_repl does not announce or send a block whose first bytes are "AKR", and
 *  pfs_dag neither beacons nor adopts a name under "ark/" (refs are region-wide and the
 *  newer seq wins, so two nodes' indexes would eat each other). -DARK_REC_SHARED turns
 *  both off (negative control). Limit: the pfs_dag manifest of ark/rec and ark/heir
 *  (name and content id, no content) is still announced; ark/heir's content is not
 *  "AKR" (the heir key is a public key).
 *
 *  Shell: ark rec new <kind> <text> | fill <kind> <len> <seed> | set <no> <text>
 *         | setfill <no> <len> <seed> | vis <no> self|heir|public | get <no> [@seq]
 *         | sum <no> [@seq] | log <no> | ls ;  ark heir [set <64 hex> | clear | log]
 *  Test env (hosted): PKERNEL_ARK_REC_FANOUT (read only when the index is created, then
 *  kept in the descriptor), PKERNEL_ARK_REC_STOP=1 (_exit before the ark/rec version).
 *  Negative controls: -DARK_REC_OVERWRITE (a new version drops its previous id),
 *  -DARK_REC_DESC_FIRST (ark/rec before the index blocks), -DARK_REC_DEFAULT_PUBLIC.
 */
#include "kernel.h"
#include "pfs_block.h"
#include "pfs_dag.h"
#include "pfs_repl.h"
#include "ark_crypto.h"

extern char *getenv(const char *);
extern int   pfs_dur_write(const char *fname, const void *data, unsigned len);
extern int   pfs_dur_read(const char *fname, void *buf, unsigned maxlen);
extern long  time(long *);
extern void  _exit(int);

#define ARK_REC_MAGIC   0x52524B41UL   /* "AKRR" LE */
#define ARK_FRAG_MAGIC  0x46524B41UL   /* "AKRF" LE */
#define ARK_IDX_MAGIC   0x49524B41UL   /* "AKRI" LE */
#define ARK_DESC_MAGIC  0x44524B41UL   /* "AKRD" LE */
#define ARK_REC_VER     1u
#define ARK_FRAG_DATA   3584u
#define ARK_REC_HDR     68u
#define ARK_REC_FRAGS   ((PFS_BLOCK_MAX - ARK_REC_HDR) / PFS_ID_LEN)      /* 125 */
#define ARK_REC_BODY    (ARK_REC_FRAGS * ARK_FRAG_DATA)                   /* 448000 */
#define ARK_IDX_HDR     16u
#define ARK_IDX_IDS     ((PFS_BLOCK_MAX - ARK_IDX_HDR) / PFS_ID_LEN)      /* 127 */
#define ARK_DEPTH_MAX   24u

/* E3 (ark-entry.md 5): the sealed copies, the only ark blocks that leave the node */
#define AKS_REC_MAGIC   0x52534B41UL   /* "AKSR" LE */
#define AKS_FRAG_MAGIC  0x46534B41UL   /* "AKSF" LE */
#define AKS_IDX_MAGIC   0x49534B41UL   /* "AKSI" LE */
#define AKS_DESC_MAGIC  0x44534B41UL   /* "AKSD" LE */
#define AKS_INNER       32u
#define AKS_REC_HDR     148u
#define AKS_REC_AAD     132u                                              /* before the tag */
#define AKS_REC_FRAGS   ((PFS_BLOCK_MAX - AKS_REC_HDR) / PFS_ID_LEN)      /* 123 */
#define AKS_CT_MAX      (AKS_REC_FRAGS * ARK_FRAG_DATA)
#define AKS_BODY_MAX    (AKS_CT_MAX - AKS_INNER)                          /* 440800 */
#define AKS_BLK_HDR     112u
#define AKS_BLK_AAD     96u
#define AKS_BLK_CT      (PFS_BLOCK_MAX - AKS_BLK_HDR)                     /* 3984 */
#define ARK_FANOUT      ((AKS_BLK_CT - 4u) / PFS_ID_LEN)                  /* 124 */
#define ARK_CHAIN_MAX   1024u
_Static_assert(ARK_FANOUT == 124 && ARK_FANOUT <= ARK_IDX_IDS, "one fanout for both trees (audit-170 1)");

enum { VIS_SELF = 0, VIS_HEIR = 1, VIS_PUBLIC = 2 };

typedef struct __attribute__((packed)) {
    UW magic, version, rec_no, seq;
    U1 prev[PFS_ID_LEN];               /* previous version's ARK_REC id; 0 = first */
    UW time_lo, time_hi;
    U1 kind, vis, learn, _pad;
    UW len, nfrag;
    U1 frag[ARK_REC_FRAGS][PFS_ID_LEN];
} ARK_REC;
_Static_assert(sizeof(ARK_REC) == ARK_REC_HDR + ARK_REC_FRAGS * PFS_ID_LEN, "ARK_REC");

typedef struct __attribute__((packed)) {
    UW magic, len;
    U1 data[ARK_FRAG_DATA];
} ARK_FRAG;

typedef struct __attribute__((packed)) {
    UW magic, level, count, _pad;
    U1 id[ARK_IDX_IDS][PFS_ID_LEN];
} ARK_IDX;
_Static_assert(sizeof(ARK_IDX) <= PFS_BLOCK_MAX, "ARK_IDX");

typedef struct __attribute__((packed)) {
    UW magic, version, nrec, depth, fanout, _pad;
    U1 root[PFS_ID_LEN];
    U1 sroot[PFS_ID_LEN];              /* E3: the sealed tree's root AKSI */
    U1 sdesc[PFS_ID_LEN];              /* E3: the newest AKSD (what `restore` takes) */
} ARK_DESC;
#define ARK_DESC_V1 (24u + PFS_ID_LEN)  /* an index written before E3 */

/* AKSR: AAD = the first 132 bytes (magic .. wrap); the fragment ids are after the tag
 * (each names its ciphertext, which the tag covers) */
typedef struct __attribute__((packed)) {
    UW magic, version, seq;
    U1 prev[PFS_ID_LEN];               /* previous version's AKSR id */
    UW ctlen, nfrag;
    U1 wrap[ARKC_WRAP_LEN], tag[16];
    U1 frag[AKS_REC_FRAGS][PFS_ID_LEN];
} AKS_REC;
_Static_assert(sizeof(AKS_REC) == AKS_REC_HDR + AKS_REC_FRAGS * PFS_ID_LEN, "AKS_REC");

/* the inner head, sealed in front of the body: nothing of it is readable outside */
typedef struct __attribute__((packed)) {
    UW rec_no, seq;
    U1 kind, vis, learn, _pad;
    UW time_lo, time_hi, len, _r[2];
} AKS_INNER_HDR;
_Static_assert(sizeof(AKS_INNER_HDR) == AKS_INNER, "AKS_INNER");

/* AKSI (an index node, pt = count + ids) and AKSD (pt = AKS_DPT). AAD = the first 96
 * bytes (magic, version, level, ctlen, wrap). Each block has its own new box. */
typedef struct __attribute__((packed)) {
    UW magic, version, level, ctlen;
    U1 wrap[ARKC_WRAP_LEN], tag[16];
    U1 ct[AKS_BLK_CT];
} AKS_BLK;
_Static_assert(sizeof(AKS_BLK) == PFS_BLOCK_MAX, "AKS_BLK");
typedef struct __attribute__((packed)) { UW count; U1 id[ARK_FANOUT][PFS_ID_LEN]; } AKS_NPT;
typedef struct __attribute__((packed)) { UW nrec, depth, fanout, _pad; U1 root[PFS_ID_LEN]; } AKS_DPT;

static void (*P)(const char *);
static ARK_REC  rec;
static ARK_FRAG frag;
static ARK_IDX  node;
static ARK_DESC desc;
static U1       body[ARK_REC_BODY];
static U1       path[ARK_DEPTH_MAX][PFS_ID_LEN];
static AKS_REC  srec;
static AKS_BLK  sblk;
static AKS_NPT  snode;
static U1       sbuf[AKS_CT_MAX];
static U1       spath[ARK_DEPTH_MAX][PFS_ID_LEN];
static U1       chain[ARK_CHAIN_MAX][PFS_ID_LEN];
static U1       ark_priv[32], ark_pub[32];
static INT      ark_key_ok;

/* ---- small helpers ------------------------------------------------------ */
static void zero(void *p, UW n) { U1 *q = p; while (n--) *q++ = 0; }
static void cpy(void *d, const void *s, UW n) { U1 *a = d; const U1 *b = s; while (n--) *a++ = *b++; }
static INT  is0(const U1 *p) { for (UW i = 0; i < PFS_ID_LEN; i++) if (p[i]) return 0; return 1; }
static void pdec(UW v) { char b[12]; INT i = 11; b[i] = 0; do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v); P(b + i); }
static void phex(const U1 *p, UW n)
{
    static const char h[] = "0123456789abcdef"; char b[3]; b[2] = 0;
    for (UW i = 0; i < n; i++) { b[0] = h[p[i] >> 4]; b[1] = h[p[i] & 15]; P(b); }
}
static const char *visname(U1 v) { return v == VIS_SELF ? "self" : v == VIS_HEIR ? "heir" : v == VIS_PUBLIC ? "public" : "?"; }
static void err(const char *m) { P("[ark-rec] error: "); P(m); P("\r\n"); }

static const UB *skip(const UB *a, UW *al) { while (*al && (*a == ' ' || *a == '\t')) { a++; (*al)--; } return a; }
static UW wordlen(const UB *a, UW al) { UW n = 0; while (n < al && a[n] != ' ' && a[n] != '\t') n++; return n; }
static INT word_is(const UB *a, UW n, const char *w)
{
    UW i = 0; while (w[i] && i < n && a[i] == (UB)w[i]) i++;
    return i == n && w[i] == 0;
}
/* parse a decimal word; returns 0 and sets *v, or -1 */
static INT num(const UB **a, UW *al, UW *v)
{
    *a = skip(*a, al);
    UW n = wordlen(*a, *al), x = 0;
    if (n == 0 || n > 9) return -1;
    for (UW i = 0; i < n; i++) { if ((*a)[i] < '0' || (*a)[i] > '9') return -1; x = x * 10 + (UW)((*a)[i] - '0'); }
    *a += n; *al -= n; *v = x; return 0;
}

/* ---- index -------------------------------------------------------------- */
static INT desc_load(void)
{
    zero(&desc, sizeof desc);
    INT n = pfs_dag_read((const UB *)"ark/rec", 7, &desc, sizeof desc);
    if (n == PFS_E_NOTFOUND) { zero(&desc, sizeof desc); return 0; }    /* no index yet */
    if ((n != (INT)sizeof desc && n != (INT)ARK_DESC_V1) || desc.magic != ARK_DESC_MAGIC || desc.version != ARK_REC_VER
        || desc.fanout < 2 || desc.fanout > ARK_IDX_IDS || desc.depth > ARK_DEPTH_MAX) return -1;
    return 1;
}

static INT node_get(const U1 id[PFS_ID_LEN], UW level)
{
    INT n = pfs_get(id, &node, sizeof node);
    if (n < (INT)ARK_IDX_HDR || node.magic != ARK_IDX_MAGIC || node.level != level
        || node.count > desc.fanout || n != (INT)(ARK_IDX_HDR + node.count * PFS_ID_LEN)) return -1;
    return 0;
}

static UW digit(UW no, UW level)
{
    UW d = no;
    for (UW l = 0; l < level; l++) d /= desc.fanout;
    return d % desc.fanout;
}

/* newest ARK_REC id of record `no` */
static INT index_find(UW no, U1 out[PFS_ID_LEN])
{
    if (no >= desc.nrec || desc.depth == 0) return -1;
    cpy(out, desc.root, PFS_ID_LEN);
    for (UW l = desc.depth; l-- > 0;) {
        if (node_get(out, l) != 0) return -1;
        UW k = digit(no, l);
        if (k >= node.count || is0(node.id[k])) return -1;
        cpy(out, node.id[k], PFS_ID_LEN);
    }
    return 0;
}

/* Build the path for record `no` set to `rid`, leaf to root. put=0 only computes ids.
 * The new root id is left in desc.root. Returns 0 or -1. */
static INT index_walk(UW no, const U1 rid[PFS_ID_LEN], INT put)
{
    U1 child[PFS_ID_LEN];
    cpy(child, rid, PFS_ID_LEN);
    for (UW l = 0; l < desc.depth; l++) {
        if (is0(path[l])) { zero(&node, sizeof node); node.magic = ARK_IDX_MAGIC; node.level = l; }
        else if (node_get(path[l], l) != 0) return -1;
        UW k = digit(no, l);
        cpy(node.id[k], child, PFS_ID_LEN);
        if (node.count < k + 1) node.count = k + 1;
        UW blen = ARK_IDX_HDR + node.count * PFS_ID_LEN;
        if (put) { if (pfs_repl_put(&node, blen, child) != PFS_OK) return -1; }
        else pfs_id_compute(&node, blen, child);
    }
    cpy(desc.root, child, PFS_ID_LEN);
    return 0;
}

static void stop_test(void)
{
    const char *e = getenv("PKERNEL_ARK_REC_STOP");
    if (e && e[0] == '1') { P("[ark-rec] stop-test: stopped before the ark/rec version\r\n"); _exit(0); }
}

/* ---- E3: the node's ark key and the sealed blocks ------------------------ */
/* The ark key (X25519) in PKERNEL_PFS_DIR/ark.key (0600, pfs_dur_write); made from
 * getrandom the first time. Without it nothing is written (no unsealed-only records). */
static INT ark_key(void)
{
    if (ark_key_ok) return 0;
    if (pfs_dur_read("ark.key", ark_priv, 32) != 32) {
        arkc_box b;
        if (arkc_box_new(&b) != 0) return -1;
        cpy(ark_priv, b.k, 32); arkc_box_wipe(&b);
        if (pfs_dur_write("ark.key", ark_priv, 32) != 0) { zero(ark_priv, 32); return -1; }
    }
    arkc_x25519_base(ark_pub, ark_priv);
    ark_key_ok = 1;
    return 0;
}

/* a block this node does not hold yet: WANT it from the region, wait up to ~20 s */
static INT fetch(const U1 id[PFS_ID_LEN])
{
    for (UW i = 0; i < 200; i++) {
        if (pfs_has(id)) return 0;
        if (i % 20 == 0) pfs_repl_want(id);
        tk_dly_tsk(100);
    }
    return pfs_has(id) ? 0 : -1;
}

/* seal pt[0..len) under a new box wrapped to this node's key, put it; id out */
static INT sblk_put(UW magic, UW level, const void *pt, UW len, U1 id[PFS_ID_LEN])
{
    arkc_box b;
    if (len > AKS_BLK_CT || arkc_box_new(&b) != 0) return -1;
    zero(&sblk, sizeof sblk);
    sblk.magic = magic; sblk.version = ARK_REC_VER; sblk.level = level; sblk.ctlen = len;
    INT r = arkc_box_wrap(&b, ark_pub, sblk.wrap);
    cpy(sblk.ct, pt, len);
    if (r == 0) r = arkc_box_seal(&b, (const U1 *)&sblk, AKS_BLK_AAD, sblk.ct, len, sblk.ct, sblk.tag);
    arkc_box_wipe(&b);
    if (r != 0) return -1;
    return pfs_repl_put(&sblk, AKS_BLK_HDR + len, id) == PFS_OK ? 0 : -1;
}

/* fetch and open; the plaintext length, or -1 (missing, wrong form, cannot open) */
static INT sblk_get(const U1 id[PFS_ID_LEN], UW magic, UW level, void *pt, UW maxlen)
{
    if (fetch(id) != 0) return -1;
    INT n = pfs_get(id, &sblk, sizeof sblk);
    if (n < (INT)AKS_BLK_HDR || sblk.magic != magic || sblk.version != ARK_REC_VER || sblk.level != level
        || sblk.ctlen > maxlen || n != (INT)(AKS_BLK_HDR + sblk.ctlen)) return -1;
    arkc_box b;
    if (arkc_box_unwrap(&b, ark_priv, sblk.wrap) != 0) return -1;
    INT r = arkc_box_open(&b, (const U1 *)&sblk, AKS_BLK_AAD, sblk.ct, sblk.ctlen, sblk.tag, pt);
    arkc_box_wipe(&b);
    return r == 0 ? (INT)sblk.ctlen : -1;
}

static INT snode_get(const U1 id[PFS_ID_LEN], UW level, UW fanout)
{
    INT n = sblk_get(id, AKS_IDX_MAGIC, level, &snode, sizeof snode);
    if (n < 4 || snode.count > fanout || n != (INT)(4 + snode.count * PFS_ID_LEN)) return -1;
    return 0;
}

static UW sdigit(UW no, UW level, UW fanout)
{
    for (UW l = 0; l < level; l++) no /= fanout;
    return no % fanout;
}

/* the newest AKSR id of record `no` in a sealed tree */
static INT sfind(const U1 root[PFS_ID_LEN], UW depth, UW fanout, UW no, U1 out[PFS_ID_LEN])
{
    if (depth == 0 || is0(root)) return -1;
    cpy(out, root, PFS_ID_LEN);
    for (UW l = depth; l-- > 0;) {
        if (snode_get(out, l, fanout) != 0) return -1;
        UW k = sdigit(no, l, fanout);
        if (k >= snode.count || is0(snode.id[k])) return -1;
        cpy(out, snode.id[k], PFS_ID_LEN);
    }
    return 0;
}

/* The sealed tree gets the same change as the plain one (index_set): record `no` -> srid.
 * desc is the state before the change; the new sealed root goes to sroot. */
static INT sindex_update(UW no, const U1 srid[PFS_ID_LEN], U1 sroot[PFS_ID_LEN])
{
    UW depth = desc.depth, nrec = desc.nrec, f = desc.fanout;
    cpy(sroot, desc.sroot, PFS_ID_LEN);
    if (f > ARK_FANOUT) return -1;                        /* an index made before E3 */
    if (nrec && is0(sroot)) return -1;                    /* records without a sealed copy */
    if (no == nrec) {
        UW cap = 1;
        for (UW l = 0; l < depth; l++) cap *= f;
        if (depth == 0 || nrec == cap) {
            if (depth >= ARK_DEPTH_MAX) return -1;
            if (depth) {                                  /* old root -> child 0 */
                zero(&snode, sizeof snode); snode.count = 1;
                cpy(snode.id[0], sroot, PFS_ID_LEN);
                if (sblk_put(AKS_IDX_MAGIC, depth, &snode, 4 + PFS_ID_LEN, sroot) != 0) return -1;
            }
            depth++;
        }
    }
    zero(spath, sizeof spath);
    if (nrec || depth > 1) {
        U1 cur[PFS_ID_LEN];
        cpy(cur, sroot, PFS_ID_LEN);
        INT have = nrec != 0;
        for (UW l = depth; l-- > 0;) {
            if (!have) break;
            cpy(spath[l], cur, PFS_ID_LEN);
            if (snode_get(cur, l, f) != 0) return -1;
            UW k = sdigit(no, l, f);
            if (l == 0) break;
            if (k >= snode.count || is0(snode.id[k])) { have = 0; continue; }
            cpy(cur, snode.id[k], PFS_ID_LEN);
        }
    }
    U1 child[PFS_ID_LEN];
    cpy(child, srid, PFS_ID_LEN);
    for (UW l = 0; l < depth; l++) {
        if (is0(spath[l])) zero(&snode, sizeof snode);
        else if (snode_get(spath[l], l, f) != 0) return -1;
        UW k = sdigit(no, l, f);
        cpy(snode.id[k], child, PFS_ID_LEN);
        if (snode.count < k + 1) snode.count = k + 1;
        if (sblk_put(AKS_IDX_MAGIC, l, &snode, 4 + snode.count * PFS_ID_LEN, child) != 0) return -1;
    }
    cpy(sroot, child, PFS_ID_LEN);
    return 0;
}

/* the AKSD for desc (already the new nrec/depth) and the sealed root; its id -> desc.sdesc */
static INT sdesc_put(const U1 sroot[PFS_ID_LEN])
{
    cpy(desc.sroot, sroot, PFS_ID_LEN);
#ifdef ARK_REC_INDEX_LOCAL
    zero(desc.sdesc, PFS_ID_LEN);
    return 0;
#else
    AKS_DPT d;
    zero(&d, sizeof d);
    d.nrec = desc.nrec; d.depth = desc.depth; d.fanout = desc.fanout;
    cpy(d.root, sroot, PFS_ID_LEN);
    return sblk_put(AKS_DESC_MAGIC, 0, &d, sizeof d, desc.sdesc);
#endif
}

/* point record `no` (no == nrec appends) at version `rid` and save a new ark/rec */
static INT index_set(UW no, const U1 rid[PFS_ID_LEN], const U1 srid[PFS_ID_LEN])
{
    if (desc.magic != ARK_DESC_MAGIC) {
        zero(&desc, sizeof desc);
        desc.magic = ARK_DESC_MAGIC; desc.version = ARK_REC_VER; desc.fanout = ARK_FANOUT;
        const char *e = getenv("PKERNEL_ARK_REC_FANOUT");
        if (e && e[0]) {
            UW f = 0; for (const char *c = e; *c >= '0' && *c <= '9'; c++) f = f * 10 + (UW)(*c - '0');
            if (f >= 2 && f <= ARK_FANOUT) desc.fanout = f;
        }
    }
    if (no > desc.nrec) return -1;
    U1 sroot[PFS_ID_LEN];
#ifndef ARK_REC_INDEX_LOCAL
    if (sindex_update(no, srid, sroot) != 0) return -1;
#else
    (void)srid; zero(sroot, PFS_ID_LEN);                  /* negative control: no AKSI / AKSD */
#endif
    if (no == desc.nrec) {                                /* append: grow if full */
        UW cap = 1;
        for (UW l = 0; l < desc.depth; l++) cap *= desc.fanout;
        if (desc.depth == 0 || desc.nrec == cap) {
            if (desc.depth >= ARK_DEPTH_MAX) return -1;
            if (desc.depth) {                             /* old root -> child 0 */
                zero(&node, sizeof node);
                node.magic = ARK_IDX_MAGIC; node.level = desc.depth; node.count = 1;
                cpy(node.id[0], desc.root, PFS_ID_LEN);
                if (pfs_repl_put(&node, ARK_IDX_HDR + PFS_ID_LEN, desc.root) != PFS_OK) return -1;
            }
            desc.depth++;
        }
    }
    /* path ids, root to leaf (zero = a node that does not exist yet) */
    zero(path, sizeof path);
    if (desc.nrec || desc.depth > 1) {
        U1 cur[PFS_ID_LEN];
        cpy(cur, desc.root, PFS_ID_LEN);
        INT have = !(desc.nrec == 0);
        for (UW l = desc.depth; l-- > 0;) {
            if (!have) break;
            cpy(path[l], cur, PFS_ID_LEN);
            if (node_get(cur, l) != 0) return -1;
            UW k = digit(no, l);
            if (l == 0) break;
            if (k >= node.count || is0(node.id[k])) { have = 0; continue; }
            cpy(cur, node.id[k], PFS_ID_LEN);
        }
    }
    UW nrec = (no == desc.nrec) ? desc.nrec + 1 : desc.nrec;
#ifdef ARK_REC_DESC_FIRST
    {   /* negative control: the new ark/rec goes first, then the index blocks */
        ARK_DESC keep = desc;
        if (index_walk(no, rid, 0) != 0) return -1;
        desc.nrec = nrec;
        if (sdesc_put(sroot) != 0) return -1;
        if (pfs_dag_save((const UB *)"ark/rec", 7, &desc, sizeof desc) != PFS_OK) return -1;
        stop_test();
        U1 sd[PFS_ID_LEN]; cpy(sd, desc.sdesc, PFS_ID_LEN);
        desc = keep;
        if (index_walk(no, rid, 1) != 0) return -1;
        desc.nrec = nrec; cpy(desc.sroot, sroot, PFS_ID_LEN); cpy(desc.sdesc, sd, PFS_ID_LEN);
        return 0;
    }
#else
    if (index_walk(no, rid, 1) != 0) return -1;
    desc.nrec = nrec;
    if (sdesc_put(sroot) != 0) return -1;                 /* AKSD after the trees' roots */
    stop_test();
    return pfs_dag_save((const UB *)"ark/rec", 7, &desc, sizeof desc) == PFS_OK ? 0 : -1;
#endif
}

/* ---- records ------------------------------------------------------------ */
static INT rec_load(const U1 id[PFS_ID_LEN], ARK_REC *r)
{
    INT n = pfs_get(id, r, sizeof *r);
    if (n < (INT)ARK_REC_HDR || r->magic != ARK_REC_MAGIC || r->version != ARK_REC_VER
        || r->nfrag > ARK_REC_FRAGS || n != (INT)(ARK_REC_HDR + r->nfrag * PFS_ID_LEN)) return -1;
    return 0;
}

/* the version `seq` (0 = newest) of record `no` into rec; returns 0 or -1 */
static INT rec_find(UW no, UW seq)
{
    U1 id[PFS_ID_LEN];
    if (index_find(no, id) != 0 || rec_load(id, &rec) != 0) return -1;
    while (seq && rec.seq != seq) {
        if (rec.seq < seq || is0(rec.prev)) return -1;
        cpy(id, rec.prev, PFS_ID_LEN);
        if (rec_load(id, &rec) != 0) return -1;
    }
    return 0;
}

static INT body_read(const ARK_REC *r)
{
    UW off = 0;
    for (UW i = 0; i < r->nfrag; i++) {
        INT n = pfs_get(r->frag[i], &frag, sizeof frag);
        if (n < 8 || frag.magic != ARK_FRAG_MAGIC || frag.len > ARK_FRAG_DATA
            || n != (INT)(8 + frag.len) || off + frag.len > r->len) return -1;
        cpy(body + off, frag.data, frag.len); off += frag.len;
    }
    return off == r->len ? 0 : -1;
}

/* write body[0..len) as fragments into rec.frag, then rec itself; rid gets its id */
static INT rec_write(UW len, U1 rid[PFS_ID_LEN], INT new_body)
{
    if (new_body) {
        rec.len = len; rec.nfrag = (len + ARK_FRAG_DATA - 1) / ARK_FRAG_DATA;
        for (UW i = 0; i < rec.nfrag; i++) {
            UW n = len - i * ARK_FRAG_DATA; if (n > ARK_FRAG_DATA) n = ARK_FRAG_DATA;
            frag.magic = ARK_FRAG_MAGIC; frag.len = n;
            cpy(frag.data, body + i * ARK_FRAG_DATA, n);
            if (pfs_repl_put(&frag, 8 + n, rec.frag[i]) != PFS_OK) return -1;
        }
    }
    rec.magic = ARK_REC_MAGIC; rec.version = ARK_REC_VER;
    return pfs_repl_put(&rec, ARK_REC_HDR + rec.nfrag * PFS_ID_LEN, rid) == PFS_OK ? 0 : -1;
}

/* E3: seal the inner head (from rec) and body[0..rec.len) under a new box, once (nonce 0),
 * as AKSF fragments and an AKSR pointing at prev_srid; srid gets its id */
#ifdef ARK_REC_SAME_KEY
static U1 same_k[32]; static INT same_have;               /* negative control */
#endif
static INT srec_write(const U1 prev_srid[PFS_ID_LEN], U1 srid[PFS_ID_LEN])
{
    UW tot = AKS_INNER + rec.len;
    if (rec.len > AKS_BODY_MAX) return -1;
    AKS_INNER_HDR in;
    zero(&in, sizeof in);
    in.rec_no = rec.rec_no; in.seq = rec.seq; in.kind = rec.kind; in.vis = rec.vis; in.learn = rec.learn;
    in.time_lo = rec.time_lo; in.time_hi = rec.time_hi; in.len = rec.len;
    cpy(sbuf, &in, AKS_INNER); cpy(sbuf + AKS_INNER, body, rec.len);
    zero(&srec, sizeof srec);
    srec.magic = AKS_REC_MAGIC; srec.version = ARK_REC_VER; srec.seq = rec.seq;
    cpy(srec.prev, prev_srid, PFS_ID_LEN);
    srec.ctlen = tot; srec.nfrag = (tot + ARK_FRAG_DATA - 1) / ARK_FRAG_DATA;
    arkc_box b;
    if (arkc_box_new(&b) != 0) return -1;
#ifdef ARK_REC_SAME_KEY
    if (same_have) cpy(b.k, same_k, 32); else { cpy(same_k, b.k, 32); same_have = 1; }
#endif
    INT r = arkc_box_wrap(&b, ark_pub, srec.wrap);
#ifdef ARK_REC_AAD_INNER
    U1 aad[AKS_REC_AAD + 1];                              /* negative control: the first draft's AAD */
    cpy(aad, &srec, AKS_REC_AAD); aad[AKS_REC_AAD] = rec.kind;
    if (r == 0) r = arkc_box_seal(&b, aad, sizeof aad, sbuf, tot, sbuf, srec.tag);
#else
    if (r == 0) r = arkc_box_seal(&b, (const U1 *)&srec, AKS_REC_AAD, sbuf, tot, sbuf, srec.tag);
#endif
    arkc_box_wipe(&b);
    if (r != 0) return -1;
#ifdef ARK_REC_PLAIN
    cpy(sbuf, &in, AKS_INNER); cpy(sbuf + AKS_INNER, body, rec.len);   /* negative control */
#endif
    for (UW i = 0; i < srec.nfrag; i++) {
        UW n = tot - i * ARK_FRAG_DATA; if (n > ARK_FRAG_DATA) n = ARK_FRAG_DATA;
        frag.magic = AKS_FRAG_MAGIC; frag.len = n;
        cpy(frag.data, sbuf + i * ARK_FRAG_DATA, n);
        if (pfs_repl_put(&frag, 8 + n, srec.frag[i]) != PFS_OK) return -1;
    }
    return pfs_repl_put(&srec, AKS_REC_HDR + srec.nfrag * PFS_ID_LEN, srid) == PFS_OK ? 0 : -1;
}

/* the AKSR `srid` into srec (fetched if this node lacks it) */
static INT srec_load(const U1 srid[PFS_ID_LEN])
{
    if (fetch(srid) != 0) return -1;
    INT n = pfs_get(srid, &srec, sizeof srec);
    if (n < (INT)AKS_REC_HDR || srec.magic != AKS_REC_MAGIC || srec.version != ARK_REC_VER
        || srec.nfrag > AKS_REC_FRAGS || srec.ctlen < AKS_INNER || srec.ctlen > AKS_CT_MAX
        || srec.nfrag != (srec.ctlen + ARK_FRAG_DATA - 1) / ARK_FRAG_DATA
        || n != (INT)(AKS_REC_HDR + srec.nfrag * PFS_ID_LEN)) return -1;
    return 0;
}

/* open the AKSR `srid`: rec's fields (not frag ids) and body. -1 = missing or cannot open;
 * then nothing of the plaintext is left in body/rec */
static INT srec_read(const U1 srid[PFS_ID_LEN])
{
    if (srec_load(srid) != 0) return -2;                  /* -2: a block is missing */
    UW off = 0;
    for (UW i = 0; i < srec.nfrag; i++) {
        if (fetch(srec.frag[i]) != 0) return -2;
        INT n = pfs_get(srec.frag[i], &frag, sizeof frag);
        if (n < 8 || frag.magic != AKS_FRAG_MAGIC || frag.len > ARK_FRAG_DATA
            || n != (INT)(8 + frag.len) || off + frag.len > srec.ctlen) return -1;
        cpy(sbuf + off, frag.data, frag.len); off += frag.len;
    }
    if (off != srec.ctlen) return -1;
    const char *t = getenv("PKERNEL_ARK_REC_TAMPER");    /* test: one stored bit flipped */
    if (t && t[0] == '1') sbuf[srec.ctlen - 1] ^= 1;
    arkc_box b;
    if (arkc_box_unwrap(&b, ark_priv, srec.wrap) != 0) return -1;
    INT r = arkc_box_open(&b, (const U1 *)&srec, AKS_REC_AAD, sbuf, srec.ctlen, srec.tag, sbuf);
    arkc_box_wipe(&b);
    if (r != 0) return -1;
    AKS_INNER_HDR in;
    cpy(&in, sbuf, AKS_INNER);
    if (in.len != srec.ctlen - AKS_INNER || in.seq != srec.seq) { zero(sbuf, srec.ctlen); return -1; }
    zero(&rec, sizeof rec);
    rec.rec_no = in.rec_no; rec.seq = in.seq; rec.kind = in.kind; rec.vis = in.vis; rec.learn = in.learn;
    rec.time_lo = in.time_lo; rec.time_hi = in.time_hi; rec.len = in.len;
    cpy(body, sbuf + AKS_INNER, in.len);
    zero(sbuf, srec.ctlen);
    return 0;
}

/* the key fingerprint of the AKSR `srid` (8 bytes of sha256 of the record key) */
static INT srec_fp(const U1 srid[PFS_ID_LEN], U1 fp[PFS_ID_LEN])
{
    if (srec_load(srid) != 0) return -1;
    arkc_box b;
    if (arkc_box_unwrap(&b, ark_priv, srec.wrap) != 0) return -1;
    pfs_id_compute(b.k, 32, fp);
    arkc_box_wipe(&b);
    return 0;
}

static void say_ver(const char *verb)
{
    P("[ark-rec] "); P(verb); P(" no="); pdec(rec.rec_no); P(" seq="); pdec(rec.seq);
    P(" vis="); P(visname(rec.vis)); P(" learn="); P(rec.learn ? "yes" : "no");
    P(" kind="); pdec(rec.kind); P(" len="); pdec(rec.len);
}

static void fill(UW len, UW seed)
{
    for (UW i = 0; i < len; i++) body[i] = (U1)((seed + i * 7 + (i >> 9) * 13) & 255);
}

/* a new record (no == desc.nrec) or a new version of record `no` */
static void put_version(INT is_new, UW no, UW kind, UW len, INT vis)
{
    U1 rid[PFS_ID_LEN], srid[PFS_ID_LEN], sprev[PFS_ID_LEN];
    INT d = desc_load();
    if (d < 0) { err("ark/rec is not readable"); return; }
    if (ark_key() != 0) { err("no ark key (PKERNEL_PFS_DIR / getrandom); nothing written"); return; }
    if (len > AKS_BODY_MAX) { err("longer than a sealed record holds"); return; }
    zero(sprev, PFS_ID_LEN);
    if (is_new) {
        no = desc.nrec;
        zero(&rec, sizeof rec);
        rec.kind = (U1)kind;
#ifdef ARK_REC_DEFAULT_PUBLIC
        rec.vis = VIS_PUBLIC;
#else
        rec.vis = VIS_SELF;                       /* 2.4: the default is "only me" */
#endif
        rec.learn = 0;                            /* 2.6: not used for learning */
        rec.seq = 1;
    } else {
        U1 oid[PFS_ID_LEN];
        if (index_find(no, oid) != 0 || rec_load(oid, &rec) != 0) { err("no such record"); return; }
        cpy(rec.prev, oid, PFS_ID_LEN);
#ifdef ARK_REC_OVERWRITE
        zero(rec.prev, PFS_ID_LEN);
#endif
        rec.seq++;
        if (vis >= 0) {                           /* a vis version is sealed again: needs the body */
            rec.vis = (U1)vis;
            if (body_read(&rec) != 0) { err("a fragment is missing or wrong"); return; }
            len = rec.len;
        }
#ifndef ARK_REC_INDEX_LOCAL
        if (sfind(desc.sroot, desc.depth, desc.fanout, no, sprev) != 0) { err("the sealed copy is not readable"); return; }
#endif
    }
    rec.rec_no = no;
    rec.len = len;
    long t = time(0);
    rec.time_lo = (UW)t; rec.time_hi = (UW)((unsigned long)t >> 16 >> 16);
    /* 5: AKSF -> AKSR -> AKRF -> AKRR -> index leaf .. root (AKSI, AKRI) -> AKSD -> ark/rec */
    if (srec_write(sprev, srid) != 0) { err("the sealed copy was not stored"); return; }
    if (rec_write(len, rid, 1) != 0) { err("a block was not stored"); return; }
    if (index_set(no, rid, srid) != 0) { err("the index was not saved"); return; }
    say_ver(is_new ? "new" : "set"); P(" id="); phex(rid, 8); P(" sid="); phex(srid, 8); P("\r\n");   /* run_ark_rec.sh local looks for it on the other node */
}

static INT hexv(UB c);

/* `ark rec restore <AKSD id>` (ark-entry.md 5): a node with the same ark.key and no
 * records of its own fetches the sealed index and records from the region, opens them,
 * and writes its plain records and both indexes again (the AKSR/AKSF are reused) */
static void restore(const U1 sd[PFS_ID_LEN])
{
    AKS_DPT dp;
    INT d = desc_load();
    if (d < 0) { P("[ark-rec] restore error: ark/rec is not readable\r\n"); return; }
    if (d > 0 && desc.nrec) { P("[ark-rec] restore error: this node already has records\r\n"); return; }
    if (ark_key() != 0) { P("[ark-rec] restore error: no ark key\r\n"); return; }
    if (fetch(sd) != 0) { P("[ark-rec] restore error: the sealed index is not in the region\r\n"); return; }
    if (sblk_get(sd, AKS_DESC_MAGIC, 0, &dp, sizeof dp) != (INT)sizeof dp || dp.fanout < 2
        || dp.fanout > ARK_FANOUT || dp.depth > ARK_DEPTH_MAX) {
        P("[ark-rec] restore cannot open the sealed index (another ark key, or not an AKSD)\r\n"); return;
    }
    zero(&desc, sizeof desc);
    desc.magic = ARK_DESC_MAGIC; desc.version = ARK_REC_VER; desc.fanout = dp.fanout;
    UW nver = 0;
    for (UW i = 0; i < dp.nrec; i++) {
        U1 sid[PFS_ID_LEN], prid[PFS_ID_LEN];
        if (sfind(dp.root, dp.depth, dp.fanout, i, sid) != 0) { P("[ark-rec] restore error: index of record "); pdec(i); P(" missing or cannot open\r\n"); return; }
        UW m = 0;
        for (;;) {
            if (m >= ARK_CHAIN_MAX || srec_load(sid) != 0) { P("[ark-rec] restore error: a version of record "); pdec(i); P(" is missing\r\n"); return; }
            cpy(chain[m++], sid, PFS_ID_LEN);
            if (is0(srec.prev)) break;
            cpy(sid, srec.prev, PFS_ID_LEN);
        }
        zero(prid, PFS_ID_LEN);
        for (UW k = m; k-- > 0;) {
            INT rr = srec_read(chain[k]);
            if (rr == -2) { P("[ark-rec] restore error: a fragment of record "); pdec(i); P(" is missing\r\n"); return; }
            if (rr != 0 || rec.rec_no != i) { P("[ark-rec] restore cannot open a version of record "); pdec(i); P("\r\n"); return; }
            cpy(rec.prev, prid, PFS_ID_LEN);
            if (rec_write(rec.len, prid, 1) != 0) { P("[ark-rec] restore error: a block was not stored\r\n"); return; }
            nver++;
        }
        if (index_set(i, prid, chain[0]) != 0) { P("[ark-rec] restore error: the index was not saved\r\n"); return; }
    }
    P("[ark-rec] restore done nrec="); pdec(desc.nrec); P(" versions="); pdec(nver); P("\r\n");
}

/* `ark rec sync`: announce again every sealed block of the current index (AKSD, AKSI,
 * each version's AKSR and AKSF), one per 700 ms. A burst of writes outruns the peers'
 * want table (pfs_repl: 8 entries, a want that does not fit is dropped and only a new
 * announce brings it back), so a node that wrote much runs this before it may be lost. */
extern UW kdds_pub_fanout(void);
static U1 synced[256][PFS_ID_LEN];
static UW nsynced, nsync_ann, nsync_lost;
/* announce once; when it reached no peer (fanout 0: the peer list was empty for a
 * moment, seen 2026-11-09), wait and announce again, up to 10 times */
static void reann(const U1 id[PFS_ID_LEN])
{
    for (UW t = 0; t < 10; t++) {
        pfs_repl_reannounce(id);
        UW f = kdds_pub_fanout();
        nsync_ann++;
        tk_dly_tsk(700);
        if (f) return;
    }
    nsync_lost++;
}
static void sync_one(const U1 id[PFS_ID_LEN])
{
    for (UW i = 0; i < nsynced; i++) { UW k = 0; while (k < PFS_ID_LEN && synced[i][k] == id[k]) k++; if (k == PFS_ID_LEN) return; }
    if (nsynced < 256) cpy(synced[nsynced++], id, PFS_ID_LEN);
    reann(id);
}
static void cmd_sync(void)
{
    nsynced = 0; nsync_ann = 0; nsync_lost = 0;
    if (desc_load() <= 0 || is0(desc.sdesc) || ark_key() != 0) { P("[ark-rec] sync: nothing sealed\r\n"); return; }
    sync_one(desc.sdesc);
    UW nver = 0;
    for (UW i = 0; i < desc.nrec; i++) {
        /* the index nodes on record i's path, root to leaf */
        U1 cur[PFS_ID_LEN];
        cpy(cur, desc.sroot, PFS_ID_LEN);
        for (UW l = desc.depth; l-- > 0;) {
            sync_one(cur);
            if (snode_get(cur, l, desc.fanout) != 0) { P("[ark-rec] sync: index of record "); pdec(i); P(" not readable\r\n"); return; }
            UW k = sdigit(i, l, desc.fanout);
            if (k >= snode.count) { P("[ark-rec] sync: index of record "); pdec(i); P(" not readable\r\n"); return; }
            cpy(cur, snode.id[k], PFS_ID_LEN);
        }
        for (;;) {                                        /* cur = the newest AKSR */
            if (srec_load(cur) != 0) { P("[ark-rec] sync: a version of record "); pdec(i); P(" is missing\r\n"); return; }
            static U1 fr[AKS_REC_FRAGS][PFS_ID_LEN];          /* not on the shell stack */
            U1 prev[PFS_ID_LEN];
            UW nf = srec.nfrag;
            cpy(fr, srec.frag, nf * PFS_ID_LEN); cpy(prev, srec.prev, PFS_ID_LEN);
            reann(cur);
            for (UW f = 0; f < nf; f++) reann(fr[f]);
            nver++;
            if (is0(prev)) break;
            cpy(cur, prev, PFS_ID_LEN);
        }
    }
    P("[ark-rec] sync done records="); pdec(desc.nrec); P(" versions="); pdec(nver); P(" announced="); pdec(nsync_ann); P(" reached-no-peer="); pdec(nsync_lost); P("\r\n");
}

static void cmd_rec(const UB *a, UW al)
{
    a = skip(a, &al);
    UW n = wordlen(a, al);
    const UB *v = a; a += n; al -= n;
    UW x, y, z;
    if (word_is(v, n, "new") || word_is(v, n, "set")) {
        INT is_new = v[0] == 'n';
        if (num(&a, &al, &x) != 0) { err(is_new ? "usage: ark rec new <kind> <text>" : "usage: ark rec set <no> <text>"); return; }
        a = skip(a, &al);
        cpy(body, a, al);
        put_version(is_new, x, x, al, -1);
    } else if (word_is(v, n, "fill") || word_is(v, n, "setfill")) {
        INT is_new = v[0] == 'f';
        if (num(&a, &al, &x) || num(&a, &al, &y) || num(&a, &al, &z) || y > ARK_REC_BODY) { err("usage: ark rec fill <kind> <len> <seed> | setfill <no> <len> <seed>"); return; }
        fill(y, z);
        put_version(is_new, x, x, y, -1);
    } else if (word_is(v, n, "vis")) {
        if (num(&a, &al, &x) != 0) { err("usage: ark rec vis <no> self|heir|public"); return; }
        a = skip(a, &al); UW m = wordlen(a, al);
        INT vv = word_is(a, m, "self") ? VIS_SELF : word_is(a, m, "heir") ? VIS_HEIR : word_is(a, m, "public") ? VIS_PUBLIC : -1;
        if (vv < 0) { err("usage: ark rec vis <no> self|heir|public"); return; }
        put_version(0, x, 0, 0, vv);
    } else if (word_is(v, n, "get") || word_is(v, n, "sum")) {
        INT get = v[0] == 'g';
        if (num(&a, &al, &x) != 0) { err("usage: ark rec get|sum <no> [@seq]"); return; }
        y = 0; a = skip(a, &al);
        if (al && a[0] == '@') { a++; al--; if (num(&a, &al, &y) != 0) { err("bad @seq"); return; } }
        if (desc_load() <= 0 || rec_find(x, y) != 0) { P("[ark-rec] "); P(get ? "get" : "sum"); P(" no="); pdec(x); P(" not found\r\n"); return; }
        if (body_read(&rec) != 0) { err("a fragment is missing or wrong"); return; }
        if (get) {
            say_ver("get"); P(" : ");
            char c[2]; c[1] = 0;
            for (UW i = 0; i < rec.len && i < 96; i++) { c[0] = (body[i] >= 32 && body[i] < 127) ? (char)body[i] : '.'; P(c); }
        } else {
            U1 h[PFS_ID_LEN];
            pfs_id_compute(body, rec.len, h);
            P("[ark-rec] sum no="); pdec(rec.rec_no); P(" seq="); pdec(rec.seq); P(" len="); pdec(rec.len);
            P(" sha256="); phex(h, PFS_ID_LEN);
        }
        P("\r\n");
    } else if (word_is(v, n, "log")) {
        if (num(&a, &al, &x) != 0) { err("usage: ark rec log <no>"); return; }
        U1 id[PFS_ID_LEN];
        if (desc_load() <= 0 || index_find(x, id) != 0 || rec_load(id, &rec) != 0) { P("[ark-rec] log no="); pdec(x); P(" not found\r\n"); return; }
        for (;;) {
            say_ver("log"); P(" id="); phex(id, PFS_ID_LEN); P("\r\n");   /* the full id: run_ark_mix.sh X2 */
            if (is0(rec.prev)) break;
            cpy(id, rec.prev, PFS_ID_LEN);
            if (rec_load(id, &rec) != 0) { err("a previous version is missing"); break; }
        }
    } else if (word_is(v, n, "ls")) {
        INT d = desc_load();
        if (d < 0) { err("ark/rec is not readable"); return; }
        P("[ark-rec] index nrec="); pdec(desc.nrec); P(" depth="); pdec(desc.depth); P(" fanout="); pdec(desc.fanout);
        if (!is0(desc.sdesc)) { P(" sealed="); phex(desc.sdesc, PFS_ID_LEN); }
        P("\r\n");
        for (UW i = 0; i < desc.nrec; i++) {
            if (rec_find(i, 0) != 0) { P("[ark-rec]   no="); pdec(i); P(" missing\r\n"); continue; }
            P("[ark-rec]   no="); pdec(i); P(" seq="); pdec(rec.seq); P(" vis="); P(visname(rec.vis)); P(" len="); pdec(rec.len); P("\r\n");
        }
    } else if (word_is(v, n, "sget") || word_is(v, n, "keys")) {
        INT keys = v[0] == 'k';
        if (num(&a, &al, &x) != 0) { err("usage: ark rec sget <no> [@seq] | keys <no>"); return; }
        y = 0; a = skip(a, &al);
        if (al && a[0] == '@') { a++; al--; if (num(&a, &al, &y) != 0) { err("bad @seq"); return; } }
        U1 sid[PFS_ID_LEN];
        if (desc_load() <= 0 || ark_key() != 0 || sfind(desc.sroot, desc.depth, desc.fanout, x, sid) != 0) {
            P("[ark-rec] "); P(keys ? "keys" : "sget"); P(" no="); pdec(x); P(" cannot open\r\n"); return;
        }
        for (;;) {
            if (keys) {
                U1 fp[PFS_ID_LEN];
                if (srec_fp(sid, fp) != 0) { P("[ark-rec] keys no="); pdec(x); P(" cannot open\r\n"); return; }
                P("[ark-rec] key no="); pdec(x); P(" seq="); pdec(srec.seq); P(" fp="); phex(fp, 8); P("\r\n");
            } else {
                if (srec_load(sid) != 0) { P("[ark-rec] sget no="); pdec(x); P(" cannot open\r\n"); return; }
                if (y == 0 || srec.seq == y) {
                    if (srec_read(sid) != 0) { P("[ark-rec] sget no="); pdec(x); P(" cannot open\r\n"); return; }
                    U1 h[PFS_ID_LEN];
                    pfs_id_compute(body, rec.len, h);
                    P("[ark-rec] sget no="); pdec(rec.rec_no); P(" seq="); pdec(rec.seq); P(" len="); pdec(rec.len);
                    P(" sha256="); phex(h, PFS_ID_LEN); P("\r\n");
                    return;
                }
                if (srec.seq < y) { P("[ark-rec] sget no="); pdec(x); P(" not found\r\n"); return; }
            }
            if (is0(srec.prev)) break;
            cpy(sid, srec.prev, PFS_ID_LEN);
        }
        if (!keys) { P("[ark-rec] sget no="); pdec(x); P(" not found\r\n"); }
    } else if (word_is(v, n, "sync")) {
        cmd_sync();
    } else if (word_is(v, n, "restore")) {
        a = skip(a, &al);
        U1 sd[PFS_ID_LEN];
        if (wordlen(a, al) != 64) { err("usage: ark rec restore <64 hex: the sealed= id>"); return; }
        for (UW i = 0; i < PFS_ID_LEN; i++) {
            INT h = hexv(a[2 * i]), l = hexv(a[2 * i + 1]);
            if (h < 0 || l < 0) { err("usage: ark rec restore <64 hex: the sealed= id>"); return; }
            sd[i] = (U1)(h * 16 + l);
        }
        restore(sd);
    } else {
        err("usage: ark rec new|fill|set|setfill|vis|get|sum|log|ls|sget|keys|sync|restore");
    }
}

/* ---- heir (2.7: the public key in ark/heir, the identity is not checked) ---- */
static U1 heir[33];
struct heir_seq { UW seq; };
static void heir_cb(void *ctx, const char *name, UW seq, pk_node_t o)
{
    (void)o;
    const char *w = "ark/heir"; UW i = 0;
    while (w[i] && name[i] == w[i]) i++;
    if (!w[i] && !name[i]) ((struct heir_seq *)ctx)->seq = seq;
}
static INT hexv(UB c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static void cmd_heir(const UB *a, UW al)
{
    a = skip(a, &al);
    UW n = wordlen(a, al);
    const UB *v = a; a += n; al -= n;
    if (word_is(v, n, "set") || word_is(v, n, "clear")) {
        zero(heir, sizeof heir);
        if (v[0] == 's') {
            a = skip(a, &al);
            if (wordlen(a, al) != 64) { P("[ark-heir] usage: ark heir set <64 hex>\r\n"); return; }
            for (UW i = 0; i < 32; i++) {
                INT h = hexv(a[2 * i]), l = hexv(a[2 * i + 1]);
                if (h < 0 || l < 0) { P("[ark-heir] usage: ark heir set <64 hex>\r\n"); return; }
                heir[1 + i] = (U1)(h * 16 + l);
            }
            heir[0] = 1;
        }
        if (pfs_dag_save((const UB *)"ark/heir", 8, heir, sizeof heir) != PFS_OK) { P("[ark-heir] error: not saved\r\n"); return; }
        P(heir[0] ? "[ark-heir] set " : "[ark-heir] cleared"); if (heir[0]) phex(heir + 1, 4); P("\r\n");
    } else if (word_is(v, n, "log") || n == 0) {
        struct heir_seq hs = { 0 };
        (void)pfs_dag_foreach_ref(heir_cb, &hs);
        if (hs.seq == 0) { P("[ark-heir] none\r\n"); return; }
        UW from = n == 0 ? hs.seq : 1;
        for (UW s = from; s <= hs.seq; s++) {
            P("[ark-heir] seq="); pdec(s);
            if (pfs_dag_read_at((const UB *)"ark/heir", 8, s, heir, sizeof heir) != (INT)sizeof heir) P(" not readable");
            else if (heir[0]) { P(" set "); phex(heir + 1, 4); }
            else P(" cleared");
            P("\r\n");
        }
    } else {
        P("[ark-heir] usage: ark heir [set <64 hex> | clear | log]\r\n");
    }
}

/* `ark rec ...` and `ark heir ...`; returns 1 if the verb was ours */
INT ark_rec_cmd(const UB *a, UW al, void (*pr)(const char *))
{
    P = pr;
    a = skip(a, &al);
    UW n = wordlen(a, al);
    if (word_is(a, n, "rec"))  { cmd_rec(a + n, al - n);  return 1; }
    if (word_is(a, n, "heir")) { cmd_heir(a + n, al - n); return 1; }
    return 0;
}
