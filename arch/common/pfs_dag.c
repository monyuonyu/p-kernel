/*
 *  pfs_dag.c — p-fs P2: object manifests + append-only version DAG + refs.
 *
 *  Spec: docs/architecture/p-fs.md §2.3 / §3.1 / §5 (P2 row).
 *  Design notes + wire formats + the honest fork caveat: pfs_dag.h.
 *
 *  Layering (no new transport — the whole point of P2):
 *    - content bytes and manifests are ordinary content-addressed
 *      blocks stored via pfs_repl_put(); the P1 put-hook ANNOUNCEs each
 *      new block to the region and peers WANT-fetch it. P2 only adds
 *      the one mutable thing p-fs allows itself: refs.
 *    - refs gossip as periodic full-state beacons on one REGION-scoped
 *      K-DDS topic, merged last-writer-wins by manifest seq with a
 *      deterministic manifest-id tie-break. Lost beacons self-heal
 *      (state, not events); concurrent saves fork — both versions
 *      survive as blocks, the ref converges to one head (pfs_dag.h).
 *
 *  Stack discipline (feedback_hosted_relay_stack_overflow): manifest /
 *  beacon / content scratch is static, never a task-stack local.
 */

#include "pfs_dag.h"
#include "pfs_block.h"
#include "pfs_repl.h"
#include "kdds.h"
#include "drpc.h"
#include "kernel.h"

/* G24 durable backend (arch/linux/pfs_durable.c). Refs are the only
 * mutable thing p-fs has; manifests + content survive automatically as
 * content-addressed blocks (pfs_block.c persists them), but the name->head
 * table must be saved separately so "dtr/weights" etc. resolve after a
 * reboot. Externs (not a header) keep the arch/linux contract out of the
 * bare-metal include chain; compiled out when !_TK_HOSTED_LIBC_. */
#ifdef _TK_HOSTED_LIBC_
extern int pfs_dur_active(void);
extern int pfs_dur_write(const char *fname, const void *data, unsigned len);
extern int pfs_dur_read(const char *fname, void *buf, unsigned maxlen);
#endif

/* ------------------------------------------------------------------ */
/* wire-image guards (LP64 / cross-ABI: feedback_lp64_typedef_trap)    */
/* ------------------------------------------------------------------ */

_Static_assert(sizeof(UW) == 4 && sizeof(UH) == 2 && sizeof(U1) == 1,
               "pfs_dag wire types must be fixed-width on this ABI");
_Static_assert(sizeof(PFSD_MANIFEST) == 100,
               "manifest block image must be 100 bytes");
_Static_assert(sizeof(PFSD_REF_ENT)  == 56,
               "ref entry must be 56 bytes");
_Static_assert(sizeof(PFSD_REF_PKT)  == 12 + PFSD_REF_PER_PKT * 56,
               "ref beacon must be 180 bytes");
_Static_assert(sizeof(PFSD_REF_PKT)  <= KDDS_DATA_MAX,
               "ref beacon must fit a K-DDS payload");
_Static_assert(sizeof(PFSD_MANIFEST) <= PFS_BLOCK_MAX,
               "a manifest must fit in one block");

/* ------------------------------------------------------------------ */
/* output helpers (sio frame channel, like pfs_repl.c)                 */
/* ------------------------------------------------------------------ */

IMPORT void sio_send_frame(const UB *buf, INT size);

static void pd_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void pd_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { pd_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    pd_puts(&buf[i]);
}

/* first 8 id bytes as 16 hex chars — matches pfs_repl.c / `pfs ls` */
static void pd_put_id(const U1 id[PFS_ID_LEN])
{
    static const char hexd[] = "0123456789abcdef";
    char out[2 * 8 + 1];
    INT j = 0;
    for (INT i = 0; i < 8; i++) {
        out[j++] = hexd[(id[i] >> 4) & 0xF];
        out[j++] = hexd[id[i] & 0xF];
    }
    out[j] = '\0';
    pd_puts(out);
}

/* print a non-NUL-terminated name field */
static void pd_put_name(const char *name, UW nlen)
{
    if (nlen > PFS_NAME_MAX) nlen = PFS_NAME_MAX;
    sio_send_frame((const UB *)name, (INT)nlen);
}

/* ------------------------------------------------------------------ */
/* tiny libc-free helpers (arch/common rule: no <string.h>)            */
/* ------------------------------------------------------------------ */

static void pd_memcpy(void *dst, const void *src, UW n)
{
    U1 *d = (U1 *)dst; const U1 *s = (const U1 *)src;
    while (n--) *d++ = *s++;
}

static void pd_memset(void *dst, U1 v, UW n)
{
    U1 *d = (U1 *)dst;
    while (n--) *d++ = v;
}

static INT pd_id_eq(const U1 a[PFS_ID_LEN], const U1 b[PFS_ID_LEN])
{
    for (INT i = 0; i < PFS_ID_LEN; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* lexicographic id compare for the deterministic fork tie-break */
static INT pd_id_cmp(const U1 a[PFS_ID_LEN], const U1 b[PFS_ID_LEN])
{
    for (INT i = 0; i < PFS_ID_LEN; i++) {
        if (a[i] != b[i]) return (INT)a[i] - (INT)b[i];
    }
    return 0;
}

static INT pd_id_zero(const U1 id[PFS_ID_LEN])
{
    for (INT i = 0; i < PFS_ID_LEN; i++)
        if (id[i]) return 0;
    return 1;
}

static INT pd_name_eq(const char *a, UW alen, const char *b, UW blen)
{
    if (alen != blen) return 0;
    for (UW i = 0; i < alen; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* DUR-REFTAB: reflected CRC32 (poly 0xEDB88420), the same variant arkfs.c
 * uses — kept local here (no cross-TU coupling; arch/common rule). Bit-at-a-
 * time: refs.tab is tiny, so the table-free form is plenty. */
static UW pd_crc32(const void *data, UW len)
{
    const U1 *p = (const U1 *)data;
    UW crc = 0xFFFFFFFFUL;
    for (UW i = 0; i < len; i++) {
        crc ^= p[i];
        for (INT b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88420UL & (~(crc & 1UL) + 1UL));
    }
    return crc ^ 0xFFFFFFFFUL;
}

/* ------------------------------------------------------------------ */
/* module state                                                        */
/* ------------------------------------------------------------------ */

/* local ref table: name -> head manifest (the only mutable state) */
typedef struct {
    PFSD_REF_ENT e;
    U1 used;
} PFSD_REF;
static PFSD_REF refs[PFS_REF_MAX];

/* G24 durable ref table on disk ("refs.tab" under $PKERNEL_PFS_DIR):
 * a tiny header + the packed used PFSD_REF_ENT entries. Rewritten on every
 * ref mutation (atomically: pfs_dur_write does temp+rename+fsync); reloaded
 * at boot by pfs_dag_restore().
 *
 * DUR-REFTAB (🟠): refs.tab is the one persistent p-fs state that is neither
 * content-addressed nor CRC'd, so a SAME-LENGTH torn write (e.g. a half-flushed
 * sector that keeps the byte count but corrupts the body) loaded garbage
 * head/seq with no way to tell. Fix: a CRC32 over {count + entry bytes} in the
 * header; on restore a mismatch REFUSES the load (the previous good in-RAM
 * state survives) instead of adopting garbage. The magic is bumped to V2 so a
 * pre-CRC same-length file is also caught (treated as not-ours -> ignored). */
#define PFSD_REFTAB_MAGIC  0x32465450UL    /* "PTF2" LE — CRC'd ref table v2 */
#define PFSD_REFTAB_FILE   "refs.tab"
typedef struct {
    UW magic;
    UW crc;                                /* crc32 over {count + entry bytes} */
    UW count;                              /* number of PFSD_REF_ENT following */
    /* crc covers this `count` field and the entries — a contiguous run that
     * starts right here, so a torn count OR a torn entry is both caught. */
} __attribute__((packed)) PFSD_REFTAB_HDR;

#ifdef _TK_HOSTED_LIBC_
static U1 dag_loading;                      /* suppress persist during restore */
#endif
static void refs_persist(void);            /* fwd: ref_set writes through it */

static W  h_ref_pub = -1, h_ref_sub = -1;   /* K-DDS "pfs/ref"        */
static UW ref_seq;                          /* own beacon counter      */
static UW last_ref_seq[DNODE_MAX];          /* rx dedup, per source    */
static UW beacon_rr;                        /* round-robin window base */

/* static scratch — never task-stack locals (net/4KB discipline).
 * man_scratch belongs to the SHELL task (save/log/cat); sweep_man
 * belongs to the gossip task — they must not share a buffer. */
static PFSD_MANIFEST man_scratch;           /* save / log / cat walk   */
static PFSD_MANIFEST sweep_man;             /* gossip-task chain sweep */
static PFSD_REF_PKT  tx_beacon;
static PFSD_REF_PKT  rx_beacon;
static U1            cat_buf[PFS_BLOCK_MAX];

/* ------------------------------------------------------------------ */
/* ref table                                                           */
/* ------------------------------------------------------------------ */

static PFSD_REF *ref_find(const char *name, UW nlen)
{
    for (INT i = 0; i < PFS_REF_MAX; i++) {
        if (refs[i].used &&
            pd_name_eq(refs[i].e.name, refs[i].e.name_len, name, nlen))
            return &refs[i];
    }
    return 0;
}

static PFSD_REF *ref_alloc(void)
{
    for (INT i = 0; i < PFS_REF_MAX; i++)
        if (!refs[i].used) return &refs[i];
    return 0;
}

static void ref_set(PFSD_REF *r, const char *name, UW nlen,
                    const U1 head[PFS_ID_LEN], UW seq, U1 origin)
{
    pd_memset(&r->e, 0, (UW)sizeof(r->e));
    pd_memcpy(r->e.name, name, nlen);
    r->e.name_len = (U1)nlen;
    pd_memcpy(r->e.head, head, PFS_ID_LEN);
    r->e.seq    = seq;
    r->e.origin = origin;
    r->used     = 1;

    /* the one mutation in all of p-fs — persist the ref table so the
     * name survives a reboot (save() and merge() both land here). */
    refs_persist();
}

/* ------------------------------------------------------------------ */
/* durable ref table (G24) — write/restore the name->head mapping      */
/* ------------------------------------------------------------------ */

#ifdef _TK_HOSTED_LIBC_
/* scratch for the on-disk image; static (stack discipline). */
static U1 reftab_buf[sizeof(PFSD_REFTAB_HDR) + PFS_REF_MAX * sizeof(PFSD_REF_ENT)];

static void refs_persist(void)
{
    if (dag_loading || !pfs_dur_active()) return;

    PFSD_REFTAB_HDR *h = (PFSD_REFTAB_HDR *)reftab_buf;
    h->magic = PFSD_REFTAB_MAGIC;

    UW n = 0;
    U1 *out = reftab_buf + sizeof(PFSD_REFTAB_HDR);
    for (INT i = 0; i < PFS_REF_MAX; i++) {
        if (!refs[i].used) continue;
        pd_memcpy(out + n * sizeof(PFSD_REF_ENT), &refs[i].e,
                  (UW)sizeof(PFSD_REF_ENT));
        n++;
    }
    h->count = n;

    /* CRC covers {count + entry bytes} — the contiguous run from &h->count to
     * the end of the entries. A torn write that preserves the file LENGTH but
     * corrupts the body now changes this CRC, so restore can refuse it. */
    UW crc_len = (UW)sizeof(h->count) + n * (UW)sizeof(PFSD_REF_ENT);
    h->crc = pd_crc32((const U1 *)&h->count, crc_len);

    pfs_dur_write(PFSD_REFTAB_FILE, reftab_buf,
                  (unsigned)(sizeof(PFSD_REFTAB_HDR) +
                             n * sizeof(PFSD_REF_ENT)));
}
#else
static void refs_persist(void) { }
#endif

/* Reload the named-ref table from disk at boot. Manifest + content blocks
 * are already back (pfs_durable_restore ran first); this just restores the
 * name->head pointers so `pfs cat`/`dtr load` resolve again. No-op when
 * persistence is disabled or on bare metal. */
void pfs_dag_restore(void)
{
#ifdef _TK_HOSTED_LIBC_
    if (!pfs_dur_active()) return;

    INT len = pfs_dur_read(PFSD_REFTAB_FILE, reftab_buf,
                           (unsigned)sizeof reftab_buf);
    if (len < (INT)sizeof(PFSD_REFTAB_HDR)) return;

    const PFSD_REFTAB_HDR *h = (const PFSD_REFTAB_HDR *)reftab_buf;
    if (h->magic != PFSD_REFTAB_MAGIC) {
        /* Wrong/absent magic (incl. the pre-CRC v1 format): not ours — keep
         * the current in-RAM state (started empty at boot), never adopt it. */
        pd_puts("[pfs] durable: refs.tab bad magic — ignored\r\n");
        return;
    }

    /* The on-disk count drives both the length check and the CRC span, so
     * validate it BEFORE dereferencing the entry bytes it claims. */
    UW n = h->count;
    if (n > PFS_REF_MAX) {
        pd_puts("[pfs] durable: refs.tab count out of range — REJECTED\r\n");
        return;
    }
    if (len < (INT)(sizeof(PFSD_REFTAB_HDR) + n * sizeof(PFSD_REF_ENT))) {
        pd_puts("[pfs] durable: refs.tab truncated — ignored\r\n");
        return;
    }

    /* DUR-REFTAB: verify the CRC over {count + entry bytes}. A same-length torn
     * write (length preserved, body corrupted) fails here and is REFUSED — the
     * previous good in-RAM ref state survives; we never load garbage head/seq. */
    {
        UW crc_len = (UW)sizeof(h->count) + n * (UW)sizeof(PFSD_REF_ENT);
        UW want = pd_crc32((const U1 *)&h->count, crc_len);
        if (want != h->crc) {
            pd_puts("[pfs] durable: refs.tab CRC mismatch — REFUSED "
                    "(keeping previous good state)\r\n");
            return;
        }
    }

    const U1 *in = reftab_buf + sizeof(PFSD_REFTAB_HDR);
    UW loaded = 0;
    dag_loading = 1;                        /* don't rewrite while loading */
    for (UW i = 0; i < n; i++) {
        const PFSD_REF_ENT *e =
            (const PFSD_REF_ENT *)(in + i * sizeof(PFSD_REF_ENT));
        if (e->name_len == 0 || e->name_len > PFS_NAME_MAX) continue;
        PFSD_REF *r = ref_find(e->name, e->name_len);
        if (!r) r = ref_alloc();
        if (!r) break;
        ref_set(r, e->name, e->name_len, e->head, e->seq, e->origin);
        loaded++;
    }
    dag_loading = 0;

    pd_puts("[pfs] durable: restored "); pd_putdec(loaded);
    pd_puts(" named ref(s) from " PFSD_REFTAB_FILE "\r\n");
#endif
}

/* ------------------------------------------------------------------ */
/* ref beacon (publish own table, rotating window)                     */
/* ------------------------------------------------------------------ */

static void beacon_publish(void)
{
    if (drpc_my_node == 0xFF || h_ref_pub < 0) return;

    pd_memset(&tx_beacon, 0, (UW)sizeof(tx_beacon));
    tx_beacon.magic    = PFSD_REF_MAGIC;
    tx_beacon.src_node = drpc_my_node;

    /* fill up to PFSD_REF_PER_PKT used slots, round-robin so a table
     * larger than one packet still fully gossips over a few beacons */
    UW n = 0;
    for (INT k = 0; k < PFS_REF_MAX && n < PFSD_REF_PER_PKT; k++) {
        INT i = (INT)((beacon_rr + (UW)k) % PFS_REF_MAX);
        if (!refs[i].used) continue;
        pd_memcpy(&tx_beacon.ent[n], &refs[i].e, (UW)sizeof(PFSD_REF_ENT));
        n++;
    }
    beacon_rr = (beacon_rr + PFSD_REF_PER_PKT) % PFS_REF_MAX;
    if (n == 0) return;                    /* nothing named yet */

    tx_beacon.n_ent = (U1)n;
    tx_beacon.seq   = ++ref_seq;
    kdds_pub(h_ref_pub, &tx_beacon, (W)sizeof(tx_beacon));
}

/* ------------------------------------------------------------------ */
/* ref merge — LWW by manifest seq, deterministic id tie-break         */
/* (idempotent: merging the same state twice changes nothing)          */
/* ------------------------------------------------------------------ */

static void merge_entry(const PFSD_REF_ENT *in)
{
    if (in->name_len == 0 || in->name_len > PFS_NAME_MAX) return;
    if (in->seq == 0) return;

    PFSD_REF *cur = ref_find(in->name, in->name_len);
    const char *how = 0;

    if (!cur) {
        cur = ref_alloc();
        if (!cur) return;              /* table full: drop, next beacon retries */
        how = "adopted";
    } else if (in->seq > cur->e.seq) {
        how = "advanced";
    } else if (in->seq == cur->e.seq && !pd_id_eq(in->head, cur->e.head) &&
               pd_id_cmp(in->head, cur->e.head) > 0) {
        /* FORK: same seq, different manifests. Both stay alive as
         * blocks; the ref converges on the larger manifest id so every
         * node picks the same winner (honest caveat in pfs_dag.h). */
        how = "fork tie-break";
    } else {
        return;                        /* ours is as new or newer */
    }

    ref_set(cur, in->name, in->name_len, in->head, in->seq, in->origin);

    /* if we don't hold the head manifest yet, chase it via P1 WANT
     * (normally the ANNOUNCE already delivered it before the beacon) */
    if (!pfs_has(in->head)) pfs_repl_want(in->head);

    pd_puts("[pfs] ref '"); pd_put_name(in->name, in->name_len);
    pd_puts("' -> seq "); pd_putdec(in->seq);
    pd_puts(" ("); pd_puts(how);
    pd_puts(", manifest "); pd_put_id(in->head);
    pd_puts(")\r\n");
}

/* ------------------------------------------------------------------ */
/* save — content block + manifest block + local ref bump              */
/* ------------------------------------------------------------------ */

INT pfs_dag_save(const UB *name, UW nlen, const void *buf, UW len)
{
    if (!name || nlen == 0 || nlen > PFS_NAME_MAX) return PFS_E_INVAL;
    if (len > PFS_BLOCK_MAX)                       return PFS_E_TOOBIG;

    /* 1. content bytes -> content-addressed block (P1 announces it) */
    U1 content_id[PFS_ID_LEN];
    INT r = pfs_repl_put(buf, len, content_id);
    if (r != PFS_OK) return r;

    /* 2. manifest: prev = current head (zero = genesis), seq = prev+1.
     * The old manifest is untouched — saving never destroys the past. */
    PFSD_REF *cur = ref_find((const char *)name, nlen);
    pd_memset(&man_scratch, 0, (UW)sizeof(man_scratch));
    man_scratch.magic       = PFSD_MAN_MAGIC;
    man_scratch.version     = PFSD_VERSION;
    man_scratch.seq         = cur ? cur->e.seq + 1 : 1;
    if (cur) pd_memcpy(man_scratch.prev, cur->e.head, PFS_ID_LEN);
    pd_memcpy(man_scratch.content, content_id, PFS_ID_LEN);
    man_scratch.content_len = len;
    man_scratch.origin      = (drpc_my_node == 0xFF) ? PFS_ORIGIN_SELF
                                                     : drpc_my_node;
    man_scratch.name_len    = (U1)nlen;
    pd_memcpy(man_scratch.name, name, nlen);

    U1 man_id[PFS_ID_LEN];
    r = pfs_repl_put(&man_scratch, (UW)sizeof(man_scratch), man_id);
    if (r != PFS_OK) return r;

    /* 3. move the ref (the only mutation in all of p-fs) */
    if (!cur) cur = ref_alloc();
    if (!cur) return PFS_E_FULL;
    ref_set(cur, (const char *)name, nlen, man_id, man_scratch.seq,
            man_scratch.origin);

    /* 4. gossip it now; the periodic beacon repeats it forever */
    beacon_publish();

    pd_puts("[pfs] saved '"); pd_put_name((const char *)name, nlen);
    pd_puts("' seq="); pd_putdec(man_scratch.seq);
    pd_puts("  manifest="); pd_put_id(man_id);
    pd_puts("  content="); pd_put_id(content_id);
    pd_puts("  len="); pd_putdec(len);
    pd_puts("\r\n");
    return PFS_OK;
}

/* ------------------------------------------------------------------ */
/* log / cat — walk the prev chain from the head                       */
/* ------------------------------------------------------------------ */

/* fetch+validate manifest `id` into man_scratch. 1 = ok, 0 = fail
 * (prints why; sends a WANT if the block simply isn't local yet). */
static INT load_manifest(const U1 id[PFS_ID_LEN])
{
    if (!pfs_has(id)) {
        pfs_repl_want(id);
        pd_puts("  (manifest "); pd_put_id(id);
        pd_puts(" not local yet - want sent, retry)\r\n");
        return 0;
    }
    INT glen = pfs_get(id, &man_scratch, (UW)sizeof(man_scratch));
    if (glen != (INT)sizeof(man_scratch) ||
        man_scratch.magic != PFSD_MAN_MAGIC ||
        man_scratch.version != PFSD_VERSION) {
        pd_puts("  (block "); pd_put_id(id);
        pd_puts(" is not a manifest)\r\n");
        return 0;
    }
    return 1;
}

static void dag_log(const char *name, UW nlen)
{
    PFSD_REF *r = ref_find(name, nlen);
    if (!r) {
        pd_puts("[pfs] no object '"); pd_put_name(name, nlen);
        pd_puts("'\r\n");
        return;
    }

    pd_puts("[pfs] log '"); pd_put_name(name, nlen);
    pd_puts("' (newest first):\r\n");

    U1 id[PFS_ID_LEN];
    pd_memcpy(id, r->e.head, PFS_ID_LEN);
    for (INT depth = 0; depth < PFSD_LOG_MAX; depth++) {
        if (!load_manifest(id)) return;
        pd_puts("  seq="); pd_putdec(man_scratch.seq);
        pd_puts("  manifest="); pd_put_id(id);
        pd_puts("  content="); pd_put_id(man_scratch.content);
        pd_puts("  len="); pd_putdec(man_scratch.content_len);
        if (man_scratch.origin == PFS_ORIGIN_SELF) pd_puts("  origin=self");
        else { pd_puts("  origin=n"); pd_putdec(man_scratch.origin); }
        pd_puts("\r\n");
        if (pd_id_zero(man_scratch.prev)) return;     /* genesis */
        pd_memcpy(id, man_scratch.prev, PFS_ID_LEN);
    }
    pd_puts("  (chain longer than PFSD_LOG_MAX - stopped)\r\n");
}

/* at_seq < 0: head version; else walk back to that seq */
static void dag_cat(const char *name, UW nlen, W at_seq)
{
    PFSD_REF *r = ref_find(name, nlen);
    if (!r) {
        pd_puts("[pfs] no object '"); pd_put_name(name, nlen);
        pd_puts("'\r\n");
        return;
    }

    U1 id[PFS_ID_LEN];
    pd_memcpy(id, r->e.head, PFS_ID_LEN);
    for (INT depth = 0; depth < PFSD_LOG_MAX; depth++) {
        if (!load_manifest(id)) return;
        if (at_seq < 0 || man_scratch.seq == (UW)at_seq) break;
        if (man_scratch.seq < (UW)at_seq || pd_id_zero(man_scratch.prev)) {
            pd_puts("[pfs] no seq "); pd_putdec((UW)at_seq);
            pd_puts(" in '"); pd_put_name(name, nlen);
            pd_puts("'\r\n");
            return;
        }
        pd_memcpy(id, man_scratch.prev, PFS_ID_LEN);
    }

    INT glen = pfs_get(man_scratch.content, cat_buf, PFS_BLOCK_MAX);
    if (glen < 0) {
        pfs_repl_want(man_scratch.content);
        pd_puts("[pfs] content "); pd_put_id(man_scratch.content);
        pd_puts(" not local yet - want sent, retry\r\n");
        return;
    }
    if (glen > (INT)PFS_BLOCK_MAX) glen = PFS_BLOCK_MAX;

    pd_puts("[pfs] cat '"); pd_put_name(name, nlen);
    pd_puts("' seq="); pd_putdec(man_scratch.seq);
    pd_puts(": ");
    sio_send_frame(cat_buf, glen);
    pd_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* read — programmatic head-content fetch (R3a: `dtr load` pulls the   */
/* trained-weights blob through the same ref -> manifest -> content    */
/* walk dag_cat uses, silently). Shell-task context only (shares       */
/* man_scratch with save/log/cat).                                     */
/* ------------------------------------------------------------------ */

INT pfs_dag_read(const UB *name, UW nlen, void *buf, UW maxlen)
{
    if (!name || nlen == 0 || nlen > PFS_NAME_MAX || !buf)
        return PFS_E_INVAL;

    PFSD_REF *r = ref_find((const char *)name, nlen);
    if (!r) return PFS_E_NOTFOUND;

    if (!pfs_has(r->e.head)) {           /* head manifest not local yet */
        pfs_repl_want(r->e.head);
        return PFS_E_NOTFOUND;
    }
    INT glen = pfs_get(r->e.head, &man_scratch, (UW)sizeof(man_scratch));
    if (glen != (INT)sizeof(man_scratch) ||
        man_scratch.magic   != PFSD_MAN_MAGIC ||
        man_scratch.version != PFSD_VERSION)
        return PFS_E_INVAL;

    INT clen = pfs_get(man_scratch.content, buf, maxlen);
    if (clen < 0) {                      /* content not local yet */
        pfs_repl_want(man_scratch.content);
        return PFS_E_NOTFOUND;
    }
    return clen;
}

/* ------------------------------------------------------------------ */
/* read_at — programmatic read of a SPECIFIC version <seq> (selfc-ring3 */
/* §1.3 rollback: run the content at seq-1). Walks the manifest         */
/* prev-chain back from the head, same as dag_cat's @seq path, but with */
/* its OWN scratch (rd_at_man) so a supervisor task can call it without  */
/* clobbering the shell's man_scratch, and silently (a WANT, no print).  */
/* ------------------------------------------------------------------ */

static PFSD_MANIFEST rd_at_man;          /* read_at-private manifest scratch */

INT pfs_dag_read_at(const UB *name, UW nlen, UW seq, void *buf, UW maxlen)
{
    if (!name || nlen == 0 || nlen > PFS_NAME_MAX || !buf || seq == 0)
        return PFS_E_INVAL;

    PFSD_REF *r = ref_find((const char *)name, nlen);
    if (!r) return PFS_E_NOTFOUND;

    U1 id[PFS_ID_LEN];
    pd_memcpy(id, r->e.head, PFS_ID_LEN);
    for (INT depth = 0; depth < PFSD_LOG_MAX; depth++) {
        if (!pfs_has(id)) { pfs_repl_want(id); return PFS_E_NOTFOUND; }
        INT glen = pfs_get(id, &rd_at_man, (UW)sizeof(rd_at_man));
        if (glen != (INT)sizeof(rd_at_man) ||
            rd_at_man.magic   != PFSD_MAN_MAGIC ||
            rd_at_man.version != PFSD_VERSION)
            return PFS_E_INVAL;
        if (rd_at_man.seq == seq) {                   /* found target version */
            INT clen = pfs_get(rd_at_man.content, buf, maxlen);
            if (clen < 0) { pfs_repl_want(rd_at_man.content); return PFS_E_NOTFOUND; }
            return clen;
        }
        if (rd_at_man.seq < seq || pd_id_zero(rd_at_man.prev))
            return PFS_E_NOTFOUND;                     /* seq not on this chain */
        pd_memcpy(id, rd_at_man.prev, PFS_ID_LEN);
    }
    return PFS_E_NOTFOUND;                             /* chain longer than LOG_MAX */
}

/* ------------------------------------------------------------------ */
/* shell dispatcher — "save <name> <text>" / "log <name>" /            */
/* "cat <name> [@<seq>]" (args points at the verb)                     */
/* ------------------------------------------------------------------ */

static const UB *skip_ws(const UB *p, const UB *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

/* token [p, t) up to whitespace; returns t */
static const UB *token_end(const UB *p, const UB *end)
{
    while (p < end && *p != ' ' && *p != '\t') p++;
    return p;
}

static INT tok_is(const UB *p, const UB *e, const char *kw)
{
    INT i = 0;
    while (kw[i]) { if (p + i >= e || p[i] != (UB)kw[i]) return 0; i++; }
    return (p + i == e);
}

void pfs_dag_cmd(const UB *args, UW len)
{
    const UB *end = args + len;
    const UB *p   = skip_ws(args, end);
    const UB *ve  = token_end(p, end);          /* verb */

    INT is_save = tok_is(p, ve, "save");
    INT is_log  = tok_is(p, ve, "log");
    INT is_cat  = tok_is(p, ve, "cat");
    if (!is_save && !is_log && !is_cat) goto usage;

    const UB *nm = skip_ws(ve, end);            /* name */
    const UB *ne = token_end(nm, end);
    UW nlen = (UW)(ne - nm);
    if (nlen == 0 || nlen > PFS_NAME_MAX) goto usage;

    if (is_save) {
        const UB *tx = skip_ws(ne, end);        /* rest of line = text */
        if (tx >= end) goto usage;
        INT r = pfs_dag_save(nm, nlen, tx, (UW)(end - tx));
        if (r != PFS_OK) {
            pd_puts("[pfs] save failed ("); pd_putdec((UW)(-r));
            pd_puts(")\r\n");
        }
        return;
    }
    if (is_log) {
        dag_log((const char *)nm, nlen);
        return;
    }
    /* cat: optional "@<seq>" */
    {
        W at = -1;
        const UB *q = skip_ws(ne, end);
        if (q < end && *q == '@') {
            q++;
            if (q >= end || *q < '0' || *q > '9') goto usage;
            W v = 0;
            while (q < end && *q >= '0' && *q <= '9') {
                v = v * 10 + (W)(*q - '0'); q++;
            }
            at = v;
        }
        dag_cat((const char *)nm, nlen, at);
        return;
    }

usage:
    pd_puts("usage: pfs save <name> <text> | pfs log <name>"
            " | pfs cat <name> [@<seq>]\r\n");
}

/* ------------------------------------------------------------------ */
/* chain sweep — chase blocks the announce plane lost                  */
/*                                                                     */
/* One save makes TWO back-to-back puts (content, then manifest), and  */
/* P1's "pfs/ann" topic is a single LATEST_ONLY slot — the manifest    */
/* announce can overwrite the content announce before a peer's poll,   */
/* so the content is never WANTed. State-based repair: every beacon    */
/* tick, walk each ref's prev chain and WANT whatever block is missing */
/* (manifest or content). Re-issued each tick until it lands, so even  */
/* the pending-table give-up self-heals. No center: each node chases   */
/* only what its own refs say it should hold.                          */
/* ------------------------------------------------------------------ */

static void sweep_missing(void)
{
    for (INT i = 0; i < PFS_REF_MAX; i++) {
        if (!refs[i].used) continue;
        U1 id[PFS_ID_LEN];
        pd_memcpy(id, refs[i].e.head, PFS_ID_LEN);
        for (INT depth = 0; depth < PFSD_LOG_MAX; depth++) {
            if (!pfs_has(id)) {                /* missing manifest */
                pfs_repl_want(id);
                break;                         /* can't see past it yet */
            }
            INT glen = pfs_get(id, &sweep_man, (UW)sizeof(sweep_man));
            if (glen != (INT)sizeof(sweep_man) ||
                sweep_man.magic != PFSD_MAN_MAGIC) break;
            if (!pfs_has(sweep_man.content))   /* missing content */
                pfs_repl_want(sweep_man.content);
            if (pd_id_zero(sweep_man.prev)) break;
            pd_memcpy(id, sweep_man.prev, PFS_ID_LEN);
        }
    }
}

/* ------------------------------------------------------------------ */
/* gossip task — beacon own refs, merge peers' (symmetric, no center)  */
/* ------------------------------------------------------------------ */

void pfs_dag_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    tk_dly_tsk(2000);                  /* let SWIM find some peers */

    UW since_beacon = PFSD_BEACON_MS;  /* beacon on first pass */
    for (;;) {
        /* merge any peer beacon sitting in the LATEST_ONLY slot */
        W r = kdds_sub(h_ref_sub, &rx_beacon, (W)sizeof(rx_beacon), 0);
        if (r >= (W)sizeof(rx_beacon) &&
            rx_beacon.magic == PFSD_REF_MAGIC &&
            rx_beacon.src_node < DNODE_MAX &&
            rx_beacon.src_node != drpc_my_node &&
            rx_beacon.seq != last_ref_seq[rx_beacon.src_node]) {
            last_ref_seq[rx_beacon.src_node] = rx_beacon.seq;
            UW n = rx_beacon.n_ent;
            if (n > PFSD_REF_PER_PKT) n = PFSD_REF_PER_PKT;
            for (UW i = 0; i < n; i++)
                merge_entry(&rx_beacon.ent[i]);
        }

        since_beacon += PFSD_POLL_MS;
        if (since_beacon >= PFSD_BEACON_MS) {
            since_beacon = 0;
            beacon_publish();
            sweep_missing();
        }
        tk_dly_tsk(PFSD_POLL_MS);
    }
}

/* ------------------------------------------------------------------ */
/* init                                                                */
/* ------------------------------------------------------------------ */

void pfs_dag_init(void)
{
    pd_memset(refs,         0, (UW)sizeof(refs));
    pd_memset(last_ref_seq, 0, (UW)sizeof(last_ref_seq));
    ref_seq   = 0;
    beacon_rr = 0;

    h_ref_pub = kdds_open_scoped(PFSD_TOPIC_REF, KDDS_QOS_LATEST_ONLY,
                                 KDDS_SCOPE_REGION);
    h_ref_sub = kdds_open_scoped(PFSD_TOPIC_REF, KDDS_QOS_LATEST_ONLY,
                                 KDDS_SCOPE_REGION);

    pd_puts("[pfs] P2 version DAG ready (refs gossip on " PFSD_TOPIC_REF
            ", region)\r\n");
}

/* ------------------------------------------------------------------ */
/* DUR-REFTAB cert — a same-length torn refs.tab must be REFUSED at     */
/* load (CRC), and a clean refs.tab must round-trip. Drives the real    */
/* refs_persist (via ref_set) and pfs_dag_restore production paths.     */
/* ------------------------------------------------------------------ */

INT pfs_dag_self_test(void (*emit)(const char *))
{
#ifdef _TK_HOSTED_LIBC_
    INT fails = 0;

    if (!pfs_dur_active()) {
        emit("[pfs-dagrefs] no durable backend active (set PKERNEL_PFS_DIR) — "
             "cert cannot run\r\n");
        emit("[pfs-dagrefs] FAIL (durable not active)\r\n");
        return 1;
    }

    static const char SENT[] = "__durtest__";
    UW snlen = (UW)(sizeof(SENT) - 1);

    /* A distinctive head id so we can tell "clean reload" from "garbage". */
    U1 good_head[PFS_ID_LEN];
    for (INT i = 0; i < PFS_ID_LEN; i++) good_head[i] = (U1)(0xC0 + i);

    /* (A) write a clean refs.tab through the production persist path. */
    PFSD_REF *r = ref_find(SENT, snlen);
    if (!r) r = ref_alloc();
    if (!r) {
        emit("[pfs-dagrefs] FAIL ref table full — cannot run cert\r\n");
        return 1;
    }
    ref_set(r, SENT, snlen, good_head, 7, PFS_ORIGIN_SELF);   /* -> refs_persist */

    /* (B) clean round-trip: read refs.tab back and verify magic + CRC. */
    INT len = pfs_dur_read(PFSD_REFTAB_FILE, reftab_buf, (unsigned)sizeof reftab_buf);
    if (len < (INT)sizeof(PFSD_REFTAB_HDR)) {
        emit("[pfs-dagrefs] FAIL refs.tab not written\r\n");
        return 1;
    }
    {
        const PFSD_REFTAB_HDR *h = (const PFSD_REFTAB_HDR *)reftab_buf;
        UW crc_len = (UW)sizeof(h->count) + h->count * (UW)sizeof(PFSD_REF_ENT);
        UW have = pd_crc32((const U1 *)&h->count, crc_len);
        if (h->magic == PFSD_REFTAB_MAGIC && have == h->crc) {
            emit("[pfs-dagrefs] ok  clean refs.tab carries a valid CRC\r\n");
        } else {
            emit("[pfs-dagrefs] FAIL clean refs.tab CRC did not verify\r\n");
            fails++;
        }
    }

    /* Clobber the LIVE head with a sentinel so a successful clean reload is
     * observable (restore overwrites it back to good_head). */
    {
        PFSD_REF *lr = ref_find(SENT, snlen);
        if (lr) { for (INT i = 0; i < PFS_ID_LEN; i++) lr->e.head[i] = 0x11; }
    }

    /* (C) clean restore reloads the good head from disk. */
    pfs_dag_restore();
    {
        PFSD_REF *lr = ref_find(SENT, snlen);
        INT okhead = lr ? 1 : 0;
        if (lr) for (INT i = 0; i < PFS_ID_LEN; i++)
            if (lr->e.head[i] != good_head[i]) { okhead = 0; break; }
        if (okhead) emit("[pfs-dagrefs] ok  clean refs.tab round-trips (head reloaded)\r\n");
        else { emit("[pfs-dagrefs] FAIL clean refs.tab did not round-trip\r\n"); fails++; }
    }

    /* (D) SAME-LENGTH torn write: flip ONE body byte, keep the file length,
     * write it straight to disk (bypassing refs_persist's CRC recompute) so
     * the stored CRC no longer matches the body — exactly a torn sector. */
    len = pfs_dur_read(PFSD_REFTAB_FILE, reftab_buf, (unsigned)sizeof reftab_buf);
    if (len <= (INT)sizeof(PFSD_REFTAB_HDR)) {
        emit("[pfs-dagrefs] FAIL refs.tab body missing for torn test\r\n");
        return fails + 1;
    }
    {
        /* flip a byte in the FIRST entry's head (a value a blind load would
         * adopt as garbage) — same length, different CRC. */
        U1 *body = reftab_buf + sizeof(PFSD_REFTAB_HDR);
        body[0] ^= 0xFF;
        (void)pfs_dur_write(PFSD_REFTAB_FILE, reftab_buf, (unsigned)len);
    }

    /* Plant a sentinel in the LIVE head; a correct restore must REFUSE the
     * torn file and leave this sentinel (NOT adopt the flipped garbage). */
    {
        PFSD_REF *lr = ref_find(SENT, snlen);
        if (lr) for (INT i = 0; i < PFS_ID_LEN; i++) lr->e.head[i] = 0x22;
    }

    pfs_dag_restore();         /* must hit the CRC-mismatch REFUSE branch */
    {
        PFSD_REF *lr = ref_find(SENT, snlen);
        INT untouched = lr ? 1 : 0;
        if (lr) for (INT i = 0; i < PFS_ID_LEN; i++)
            if (lr->e.head[i] != 0x22) { untouched = 0; break; }
        if (untouched)
            emit("[pfs-dagrefs] ok  torn refs.tab REFUSED — garbage not adopted\r\n");
        else {
            emit("[pfs-dagrefs] FAIL torn refs.tab loaded garbage head/seq\r\n");
            fails++;
        }
    }

    /* Leave disk in a good state for any later cert / reboot. */
    {
        PFSD_REF *cr = ref_find(SENT, snlen);
        if (!cr) cr = ref_alloc();
        if (cr) ref_set(cr, SENT, snlen, good_head, 7, PFS_ORIGIN_SELF);
    }

    if (fails == 0)
        emit("[pfs-dagrefs] PASS (clean round-trips, torn refused)\r\n");
    else
        emit("[pfs-dagrefs] FAIL\r\n");
    return fails;
#else
    (void)emit;
    return 0;     /* bare metal: no durable ref table */
#endif
}
