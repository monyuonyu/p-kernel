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
 *  Nothing of the ark leaves the node yet (2.5: only sealed records may, and sealing is
 *  E3): pfs_repl does not announce or send a block whose first bytes are "AKR", and
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

extern char *getenv(const char *);
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
} ARK_DESC;

static void (*P)(const char *);
static ARK_REC  rec;
static ARK_FRAG frag;
static ARK_IDX  node;
static ARK_DESC desc;
static U1       body[ARK_REC_BODY];
static U1       path[ARK_DEPTH_MAX][PFS_ID_LEN];

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
    INT n = pfs_dag_read((const UB *)"ark/rec", 7, &desc, sizeof desc);
    if (n == PFS_E_NOTFOUND) { zero(&desc, sizeof desc); return 0; }    /* no index yet */
    if (n != (INT)sizeof desc || desc.magic != ARK_DESC_MAGIC || desc.version != ARK_REC_VER
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

/* point record `no` (no == nrec appends) at version `rid` and save a new ark/rec */
static INT index_set(UW no, const U1 rid[PFS_ID_LEN])
{
    if (desc.magic != ARK_DESC_MAGIC) {
        zero(&desc, sizeof desc);
        desc.magic = ARK_DESC_MAGIC; desc.version = ARK_REC_VER; desc.fanout = ARK_IDX_IDS;
        const char *e = getenv("PKERNEL_ARK_REC_FANOUT");
        if (e && e[0]) {
            UW f = 0; for (const char *c = e; *c >= '0' && *c <= '9'; c++) f = f * 10 + (UW)(*c - '0');
            if (f >= 2 && f <= ARK_IDX_IDS) desc.fanout = f;
        }
    }
    if (no > desc.nrec) return -1;
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
        if (pfs_dag_save((const UB *)"ark/rec", 7, &desc, sizeof desc) != PFS_OK) return -1;
        stop_test();
        desc = keep;
        if (index_walk(no, rid, 1) != 0) return -1;
        desc.nrec = nrec;
        return 0;
    }
#else
    if (index_walk(no, rid, 1) != 0) return -1;
    stop_test();
    desc.nrec = nrec;
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
    long t = time(0);
    rec.magic = ARK_REC_MAGIC; rec.version = ARK_REC_VER;
    rec.time_lo = (UW)t; rec.time_hi = (UW)((unsigned long)t >> 16 >> 16);
    return pfs_repl_put(&rec, ARK_REC_HDR + rec.nfrag * PFS_ID_LEN, rid) == PFS_OK ? 0 : -1;
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
    U1 rid[PFS_ID_LEN];
    INT d = desc_load();
    if (d < 0) { err("ark/rec is not readable"); return; }
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
        if (vis >= 0) rec.vis = (U1)vis;
    }
    rec.rec_no = no;
    if (rec_write(len, rid, vis < 0) != 0) { err("a block was not stored"); return; }
    if (index_set(no, rid) != 0) { err("the index was not saved"); return; }
    say_ver(is_new ? "new" : "set"); P("\r\n");
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
        if (desc_load() <= 0 || rec_find(x, 0) != 0) { P("[ark-rec] log no="); pdec(x); P(" not found\r\n"); return; }
        for (;;) {
            say_ver("log"); P("\r\n");
            if (is0(rec.prev)) break;
            U1 id[PFS_ID_LEN]; cpy(id, rec.prev, PFS_ID_LEN);
            if (rec_load(id, &rec) != 0) { err("a previous version is missing"); break; }
        }
    } else if (word_is(v, n, "ls")) {
        INT d = desc_load();
        if (d < 0) { err("ark/rec is not readable"); return; }
        P("[ark-rec] index nrec="); pdec(desc.nrec); P(" depth="); pdec(desc.depth); P(" fanout="); pdec(desc.fanout); P("\r\n");
        for (UW i = 0; i < desc.nrec; i++) {
            if (rec_find(i, 0) != 0) { P("[ark-rec]   no="); pdec(i); P(" missing\r\n"); continue; }
            P("[ark-rec]   no="); pdec(i); P(" seq="); pdec(rec.seq); P(" vis="); P(visname(rec.vis)); P(" len="); pdec(rec.len); P("\r\n");
        }
    } else {
        err("usage: ark rec new|fill|set|setfill|vis|get|sum|log|ls");
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
