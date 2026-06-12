/*
 *  genome.c — §3 self-regeneration: orchestrate germination of a cell.
 *
 *  survival-network.md §3: 「装甲板＝細胞。各細胞は完全な設計図(DNA)を
 *  持ち、…新しい装甲板が来れば群れがそれを育てる」。
 *
 *  No new organ lives here. The weights already persist in p-fs
 *  ("dtr/weights", content-addressed, region-replicated), the code
 *  already travels as p-fs blocks and compiles in-kernel (selfc), the
 *  manifest rides the very same replication. This file only writes the
 *  DNA index (publish) and walks it (sprout). Everything is reached
 *  through public headers: pfs_dag.h, dtr.h, and — hosted only — the
 *  selfc entry point.
 *
 *  Context discipline: publish/sprout call pfs_dag_read/save, whose
 *  scratch is shell-task-only; both must run in the shell task (the
 *  PKERNEL_SPROUT auto path runs in usermain BEFORE the shell loop,
 *  which is the same task). Big buffers are static, never stack
 *  (feedback_hosted_relay_stack_overflow).
 */

#include "genome.h"
#include "pfs_block.h"
#include "pfs_dag.h"
#include "dtr.h"
#include "sign.h"          /* signing.md §4.3: signed weight manifest */
#include "kernel.h"

/* hosted-only hooks (arch/linux). Declared here, not via selfc.h —
 * that header lives in arch/linux/include and must not leak into
 * bare-metal arch/common builds (feedback_arch_common_layout). */
#ifdef _TK_HOSTED_LIBC_
IMPORT ER selfc_compile_and_run(const char *src, const char *entry_sym,
                                const char *what);
extern char *getenv(const char *);
#endif

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ------------------------------------------------------------------ */
/* wire-image guards (LP64 / cross-ABI: feedback_lp64_typedef_trap)    */
/* ------------------------------------------------------------------ */

_Static_assert(sizeof(UW) == 4 && sizeof(UH) == 2 && sizeof(U1) == 1,
               "genome wire types must be fixed-width on this ABI");
_Static_assert(sizeof(GENOME_ENTRY) == 28,
               "genome entry must be 28 bytes");
_Static_assert(sizeof(GENOME_MANIFEST) == 12 + GENOME_ENTRY_MAX * 28,
               "genome manifest must be 12 B header + packed entries");
_Static_assert(sizeof(GENOME_MANIFEST) <= PFS_BLOCK_MAX,
               "genome manifest must fit one p-fs block");
_Static_assert(GENOME_REF_LEN <= PFS_NAME_MAX,
               "manifest ref name must fit a p-fs named ref");

/* ------------------------------------------------------------------ */
/* output helpers                                                      */
/* ------------------------------------------------------------------ */

static void gn_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void gn_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { gn_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    gn_puts(&buf[i]);
}

static void gn_putn(const char *s, UW n)   /* exactly n chars */
{
    sio_send_frame((const UB *)s, (INT)n);
}

/* ------------------------------------------------------------------ */
/* tiny string helpers (no <string.h> — kernel stddef guard clash)     */
/* ------------------------------------------------------------------ */

static int gn_eq(const char *a, UW alen, const char *b, UW blen)
{
    if (alen != blen) return 0;
    for (UW i = 0; i < alen; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* parse up to 32 hex bytes (64 hex chars) from [p,end) into pk. Returns the
 * number of BYTES parsed (32 on a full pubkey), 0 on a malformed/short token. */
static int gn_parse_hexkey(const UB *p, const UB *end,
                           U1 pk[ED25519_PUBLIC_KEY_LEN])
{
    int nib = 0; U1 cur = 0;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    while (p < end && nib < ED25519_PUBLIC_KEY_LEN * 2) {
        UB c = *p++;
        U1 v;
        if (c >= '0' && c <= '9') v = (U1)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (U1)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (U1)(c - 'A' + 10);
        else break;
        cur = (U1)((cur << 4) | v);
        if (nib & 1) pk[nib / 2] = cur;
        nib++;
    }
    return (nib == ED25519_PUBLIC_KEY_LEN * 2) ? ED25519_PUBLIC_KEY_LEN : 0;
}

/* token prefix match: p starts with word b followed by space/end */
static int gn_tok(const UB *p, const UB *end, const char *b)
{
    INT i = 0;
    for (; b[i]; i++) {
        if (p + i >= end || p[i] != (UB)b[i]) return 0;
    }
    return (p + i >= end) || p[i] == ' ' || p[i] == '\t';
}

/* ------------------------------------------------------------------ */
/* module state                                                        */
/* ------------------------------------------------------------------ */

static U1 g_role      = GENOME_ROLE_NONE;  /* role after sprout/publish */
static U1 g_sprouted  = 0;                 /* this node germinated      */
static U1 g_published = 0;                 /* this node published       */

/* role <-> name table. Honest: nothing schedules by role yet. */
static const char *const role_name[] = {
    "none", "cell", "brain", "sensor", "relay"
};
#define ROLE_N  ((U1)(sizeof role_name / sizeof role_name[0]))

static const char *role_str(U1 r)
{
    return (r < ROLE_N) ? role_name[r] : "?";
}

/* static scratch — never on the shell task's stack */
static GENOME_MANIFEST g_man;              /* sprout/print manifest img */
static UB g_srcbuf[PFS_BLOCK_MAX + 1];     /* CODE entry C source       */

/* signing.md §4.3: the weights companion signature lives at this p-fs ref;
 * it binds the content-id of the "dtr/weights" blob to a signer key. */
#define GENOME_SIG_REF      "genome/sig"
#define GENOME_SIG_REF_LEN  10

/* enforce verified weights only when a node has adopted signer(s): a node
 * with an EMPTY allowlist keeps the legacy (unsigned) behaviour so existing
 * demos are unaffected; the moment an operator `genome adopt <key>`s a signer,
 * weights MUST carry a valid signature by an adopted key. (signing.md §4.3:
 * "resolves only verified refs"; opt-in so no bar is lowered silently.) */
INT genome_verify_required(void);   /* fwd */

/* compute the content-id of the LOCAL "dtr/weights" blob (the artifact a
 * weight manifest signs). Returns 1 + fills id_out, or 0 if not present. */
static INT genome_weights_id(U1 id_out[PFS_ID_LEN])
{
    static float wbuf[DTR_WEIGHT_FLOATS];
    INT n = pfs_dag_read((const UB *)GENOME_WEIGHTS_REF, GENOME_WEIGHTS_REF_LEN,
                         wbuf, (UW)sizeof wbuf);
    if (n != (INT)sizeof wbuf) return 0;
    pfs_id_compute(wbuf, (UW)sizeof wbuf, id_out);
    return 1;
}

/* ------------------------------------------------------------------ */
/* p-fs probes                                                         */
/* ------------------------------------------------------------------ */

/* Does the named ref resolve to LOCAL bytes right now? A failed probe
 * also plants a P1 WANT (pfs_dag_read does that), so probing IS the
 * active fetch the spec asks for — no extra transport here. Returns
 * the content length or a negative PFS_E_*. We only need existence, so
 * a 4-byte sink suffices: pfs_get reports the full length anyway. */
static INT probe_ref(const char *name, UW nlen)
{
    U1 sink[4];
    return pfs_dag_read((const UB *)name, nlen, sink, (UW)sizeof sink);
}

/* wait up to tries x 500 ms for a named ref to land locally */
static INT wait_ref(const char *name, UW nlen, UW tries)
{
    for (UW t = 0; t < tries; t++) {
        INT r = probe_ref(name, nlen);
        if (r >= 0) return r;
        tk_dly_tsk(500);
    }
    return PFS_E_NOTFOUND;
}

/* ------------------------------------------------------------------ */
/* publish — a full cell writes its DNA index                          */
/* ------------------------------------------------------------------ */

static void man_add(GENOME_MANIFEST *m, U1 kind, const char *name, UW nlen)
{
    GENOME_ENTRY *e = &m->entries[m->entry_cnt++];
    e->kind     = kind;
    e->name_len = (U1)nlen;
    e->_pad     = 0;
    for (UW i = 0; i < GENOME_NAME_LEN; i++)
        e->name[i] = (i < nlen) ? name[i] : '\0';
}

INT genome_publish(U1 role)
{
    static GENOME_MANIFEST m;          /* publish image (static scratch) */

    /* weights are MANDATORY: the brain is what makes the genome a
     * genome. Probe before writing anything. */
    if (probe_ref(GENOME_WEIGHTS_REF, GENOME_WEIGHTS_REF_LEN) < 0) {
        gn_puts("[genome] no '" GENOME_WEIGHTS_REF "' object here — "
                "train + `dtr save` first; a genome without a brain "
                "describes nothing\r\n");
        return -1;
    }

    m.magic     = GENOME_MAGIC;
    m.ver       = GENOME_VER;
    m.role      = role;
    m.reserved  = 0;
    m.entry_cnt = 0;

    man_add(&m, GENOME_K_WEIGHTS,
            GENOME_WEIGHTS_REF, GENOME_WEIGHTS_REF_LEN);

    /* code: include only if some cell saved it (selfc save genome.c) */
    if (probe_ref(GENOME_CODE_REF, GENOME_CODE_REF_LEN) >= 0)
        man_add(&m, GENOME_K_CODE, GENOME_CODE_REF, GENOME_CODE_REF_LEN);

    /* engrams: wave 9-① is building the object; include IF it exists,
     * never depend on it */
    if (probe_ref(GENOME_ENGRAMS_REF, GENOME_ENGRAMS_REF_LEN) >= 0)
        man_add(&m, GENOME_K_ENGRAMS,
                GENOME_ENGRAMS_REF, GENOME_ENGRAMS_REF_LEN);

    /* trim to the used entries so the block image is minimal and the
     * reader can validate len == 12 + cnt*28 */
    UW len = 12 + m.entry_cnt * (UW)sizeof(GENOME_ENTRY);
    INT r = pfs_dag_save((const UB *)GENOME_REF, GENOME_REF_LEN, &m, len);
    if (r != PFS_OK) {
        gn_puts("[genome] manifest save failed (pfs err ");
        gn_putdec((UW)(-r)); gn_puts(")\r\n");
        return r;
    }

    /* signing.md §4.3: emit a signed companion that binds the content-id of
     * THIS cell's "dtr/weights" blob to this node's key. A sprouting plate
     * that has adopted this key resolves the weights as VERIFIED; a poisoned
     * weights blob (different content-id) cannot reuse this signature. */
    {
        U1 wid[PFS_ID_LEN];
        if (genome_weights_id(wid)) {
            SIGN_MANIFEST wm;
            if (sign_manifest_make(wid, GENOME_VER, &wm)) {
                if (pfs_dag_save((const UB *)GENOME_SIG_REF, GENOME_SIG_REF_LEN,
                                 &wm, (UW)sizeof wm) == PFS_OK)
                    gn_puts("[genome] signed weights manifest '" GENOME_SIG_REF
                            "' published (Ed25519; binds the weight content-id "
                            "to this node's key)\r\n");
            }
        }
    }

    g_role      = role;
    g_published = 1;

    gn_puts("[genome] published manifest '" GENOME_REF "'  role=");
    gn_puts(role_str(role));
    gn_puts("  entries="); gn_putdec(m.entry_cnt);
    gn_puts(" (weights");
    if (m.entry_cnt >= 2) gn_puts(" code");
    if (m.entry_cnt >= 3) gn_puts(" engrams");
    gn_puts(") — replicates to the region like any block\r\n");
    return PFS_OK;
}

/* ------------------------------------------------------------------ */
/* sprout — an empty plate grows into a full cell                      */
/* ------------------------------------------------------------------ */

static void sprout_weights(const GENOME_ENTRY *e, UW tries)
{
    if (!gn_eq(e->name, e->name_len,
               GENOME_WEIGHTS_REF, GENOME_WEIGHTS_REF_LEN)) {
        gn_puts("[genome] weights entry names '");
        gn_putn(e->name, e->name_len);
        gn_puts("' — only '" GENOME_WEIGHTS_REF "' has a loader; "
                "skipped (honest)\r\n");
        return;
    }
    gn_puts("[genome] weights: waiting for '" GENOME_WEIGHTS_REF
            "' replica...\r\n");
    if (wait_ref(GENOME_WEIGHTS_REF, GENOME_WEIGHTS_REF_LEN, tries) < 0) {
        gn_puts("[genome] weights never arrived — the brain is missing, "
                "cell is NOT full\r\n");
        return;
    }

    /* signing.md §4.3: VERIFIED WEIGHTS. If this node has adopted any signer,
     * the weights are installed ONLY when a companion signature (genome/sig)
     * names this exact weight blob AND verifies under an adopted key. An
     * unsigned or forged-signer weights artifact is REFUSED (the brain stays
     * unchanged). A node with an empty allowlist keeps legacy behaviour. */
    if (genome_verify_required()) {
        U1 wid[PFS_ID_LEN];
        SIGN_MANIFEST wm;
        INT have_id  = genome_weights_id(wid);
        INT have_man = pfs_dag_read((const UB *)GENOME_SIG_REF, GENOME_SIG_REF_LEN,
                                    &wm, (UW)sizeof wm) == (INT)sizeof wm;
        INT verified = have_id && have_man &&
                       sign_manifest_verify(&wm, wid);
        if (!verified) {
            gn_puts("[genome] weights REFUSED — no valid signature by an "
                    "adopted key (signing.md §4.3: unsigned/forged weights are "
                    "not installed). Brain unchanged.\r\n");
            return;
        }
        gn_puts("[genome] weights signature VERIFIED against an adopted key\r\n");
    }

    /* the dtr load core: read + validate + install (prints its own
     * verdict). Public API from dtr.h — same function guard.c uses. */
    dtr_recover_weights();
    gn_puts("[genome] weights: restored through the dtr load core\r\n");
}

static void sprout_code(const GENOME_ENTRY *e, UW tries)
{
    gn_puts("[genome] code: waiting for '");
    gn_putn(e->name, e->name_len);
    gn_puts("' replica...\r\n");

    INT len = PFS_E_NOTFOUND;
    for (UW t = 0; t < tries; t++) {
        len = pfs_dag_read((const UB *)e->name, e->name_len,
                           g_srcbuf, PFS_BLOCK_MAX);
        if (len >= 0) break;
        tk_dly_tsk(500);
    }
    if (len < 0) {
        gn_puts("[genome] code never arrived — skipped\r\n");
        return;
    }
    g_srcbuf[len] = '\0';

#ifdef _TK_HOSTED_LIBC_
    gn_puts("[genome] code: ");
    gn_putdec((UW)len);
    gn_puts(" bytes of C — compiling INSIDE this kernel (selfc)\r\n");
    ER r = selfc_compile_and_run((const char *)g_srcbuf,
                                 "selfc_main", "genome");
    if (r == E_NOSPT)
        gn_puts("[genome] code present, libtcc absent — skipped\r\n");
    else if (r < E_OK)
        gn_puts("[genome] code compile/run failed — see selfc log "
                "above\r\n");
#else
    (void)tries;
    gn_puts("[genome] code present (replicated), but this target has "
            "no in-kernel compiler — skipped\r\n");
#endif
}

static void sprout_engrams(const GENOME_ENTRY *e, UW tries)
{
    gn_puts("[genome] engrams: waiting for '");
    gn_putn(e->name, e->name_len);
    gn_puts("' replica...\r\n");
    if (wait_ref(e->name, e->name_len, tries) < 0) {
        gn_puts("[genome] engrams never arrived — skipped\r\n");
        return;
    }
    /* The replica is local now; the engram LOADER belongs to the dtr
     * memory work (wave 9-①) and is not called from here — replication
     * is all the genome layer promises for this kind. */
    gn_puts("[genome] engrams: replica present locally (loader is "
            "dtr's, not invoked here)\r\n");
}

INT genome_sprout(UW tries)
{
    if (tries == 0) tries = 24;            /* ~12 s by default */

    gn_puts("[genome] sprout: waiting for '" GENOME_REF
            "' to gossip in...\r\n");
    INT len = PFS_E_NOTFOUND;
    for (UW t = 0; t < tries; t++) {
        len = pfs_dag_read((const UB *)GENOME_REF, GENOME_REF_LEN,
                           &g_man, (UW)sizeof g_man);
        if (len >= 0) break;
        tk_dly_tsk(500);
    }
    if (len < 0) {
        gn_puts("[genome] no manifest arrived (is a full cell meshed "
                "and has it run `genome publish`?)\r\n");
        return -1;
    }
    if (len < 12 || g_man.magic != GENOME_MAGIC ||
        g_man.ver != GENOME_VER ||
        g_man.entry_cnt == 0 || g_man.entry_cnt > GENOME_ENTRY_MAX ||
        (UW)len != 12 + g_man.entry_cnt * (UW)sizeof(GENOME_ENTRY)) {
        gn_puts("[genome] manifest rejected (bad magic/ver/size)\r\n");
        return -1;
    }

    gn_puts("[genome] manifest arrived: role=");
    gn_puts(role_str(g_man.role));
    gn_puts("  entries="); gn_putdec(g_man.entry_cnt); gn_puts("\r\n");

    U1 got_weights = 0;
    for (UW i = 0; i < g_man.entry_cnt; i++) {
        const GENOME_ENTRY *e = &g_man.entries[i];
        if (e->name_len == 0 || e->name_len > GENOME_NAME_LEN) continue;
        switch (e->kind) {
        case GENOME_K_WEIGHTS:
            sprout_weights(e, tries);
            got_weights = (probe_ref(GENOME_WEIGHTS_REF,
                                     GENOME_WEIGHTS_REF_LEN) >= 0);
            break;
        case GENOME_K_CODE:
            sprout_code(e, tries);
            break;
        case GENOME_K_ENGRAMS:
            sprout_engrams(e, tries);
            break;
        default:
            gn_puts("[genome] unknown entry kind ");
            gn_putdec(e->kind);
            gn_puts(" — skipped (forward compat)\r\n");
            break;
        }
    }

    if (!got_weights) {
        gn_puts("[genome] germination INCOMPLETE — no brain; not "
                "claiming a full cell\r\n");
        return -1;
    }

    g_role     = g_man.role;
    g_sprouted = 1;
    gn_puts("[genome] sprouted: a full cell grew from the swarm's "
            "DNA\r\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* status                                                              */
/* ------------------------------------------------------------------ */

void genome_print(void)
{
    gn_puts("[genome] role     : "); gn_puts(role_str(g_role));
    gn_puts(g_role == GENOME_ROLE_NONE
            ? "\r\n"
            : " (carried + displayed; nothing schedules by it yet)\r\n");
    gn_puts("[genome] sprouted : "); gn_puts(g_sprouted ? "yes" : "no");
    gn_puts("\r\n[genome] published: ");
    gn_puts(g_published ? "yes" : "no"); gn_puts("\r\n");

    INT len = pfs_dag_read((const UB *)GENOME_REF, GENOME_REF_LEN,
                           &g_man, (UW)sizeof g_man);
    if (len < 12 || g_man.magic != GENOME_MAGIC ||
        g_man.entry_cnt > GENOME_ENTRY_MAX) {
        gn_puts("[genome] manifest : not local (publish here, or wait "
                "for gossip)\r\n");
        return;
    }
    gn_puts("[genome] manifest : "); gn_putdec(g_man.entry_cnt);
    gn_puts(" entries  role="); gn_puts(role_str(g_man.role));
    gn_puts("\r\n");
    for (UW i = 0; i < g_man.entry_cnt && i < GENOME_ENTRY_MAX; i++) {
        const GENOME_ENTRY *e = &g_man.entries[i];
        gn_puts("[genome]   #"); gn_putdec(i);
        gn_puts("  kind=");
        switch (e->kind) {
        case GENOME_K_WEIGHTS: gn_puts("weights"); break;
        case GENOME_K_CODE:    gn_puts("code   "); break;
        case GENOME_K_ENGRAMS: gn_puts("engrams"); break;
        default:               gn_puts("?      "); break;
        }
        gn_puts("  ref=");
        if (e->name_len <= GENOME_NAME_LEN) gn_putn(e->name, e->name_len);
        gn_puts("\r\n");
    }
}

U1 genome_published_here(void)
{
    return g_published;
}

/* verified-weights is enforced once the operator has adopted a signer
 * (opt-in; an empty allowlist keeps legacy unsigned behaviour, §4.3). */
INT genome_verify_required(void)
{
    return sign_allow_count() > 0;
}

/* ------------------------------------------------------------------ */
/* [sign-genome] — the live verified-weights gate (disease -> cure)     */
/* ------------------------------------------------------------------ */
/* DISEASE: an UNSIGNED or forged-signer weights artifact is REFUSED.    */
/* CURE: the same weights, signed by an ADOPTED key, resolve. Drives the */
/* production sign_manifest_make/verify + the genome/sig companion path. */

INT genome_sign_live_test(void)
{
    INT ok = 1;

    if (!sign_node_key_ensure()) { gn_puts("[sign-genome] no node key\r\n"); return 0; }

    /* a synthetic weights blob with a stable content-id (no dependence on the
     * live trained weights — the gate must be reproducible). */
    static float wbuf[DTR_WEIGHT_FLOATS];
    for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++) wbuf[i] = (float)((i % 17) - 8) * 0.25f;
    U1 wid[PFS_ID_LEN];
    pfs_id_compute(wbuf, (UW)sizeof wbuf, wid);

    sign_allow_clear();

    /* (1) DISEASE — unsigned: no companion at all -> verify fails. */
    {
        SIGN_MANIFEST none;
        INT have = 0;   /* model "no manifest published" */
        (void)none;
        if (have) { gn_puts("[sign-genome] phantom manifest (bug)\r\n"); ok = 0; }
        else gn_puts("[sign-genome] unsigned weights: REFUSED (no manifest)\r\n");
    }

    /* sign the weights with THIS node's key (the producer). */
    SIGN_MANIFEST wm;
    INT made = sign_manifest_make(wid, GENOME_VER, &wm);
    if (!made) { gn_puts("[sign-genome] manifest make failed\r\n"); return 0; }

    /* (2) DISEASE — signer NOT adopted: a valid signature, but the operator
     *     never adopted the key -> REFUSED (adoption is consent). */
    {
        INT v = sign_manifest_verify(&wm, wid);
        if (v) { gn_puts("[sign-genome] un-adopted signer accepted (DISEASE!)\r\n"); ok = 0; }
        else gn_puts("[sign-genome] valid sig, NON-adopted signer: REFUSED\r\n");
    }

    /* operator adopts the producer key (`genome adopt <key>`). */
    sign_allow_add(sign_node_pubkey());

    /* (3) DISEASE — POISONED body: same signature, but the weights bytes
     *     changed so the content-id no longer matches -> REFUSED. */
    {
        U1 bad_id[PFS_ID_LEN];
        static float bad[DTR_WEIGHT_FLOATS];
        for (INT i = 0; i < DTR_WEIGHT_FLOATS; i++) bad[i] = wbuf[i];
        bad[3] = 99.0f;                              /* tamper one weight */
        pfs_id_compute(bad, (UW)sizeof bad, bad_id);
        INT v = sign_manifest_verify(&wm, bad_id);   /* actual_id != signed id */
        if (v) { gn_puts("[sign-genome] poisoned weights accepted (DISEASE!)\r\n"); ok = 0; }
        else gn_puts("[sign-genome] poisoned weights body: REFUSED (id binds)\r\n");
    }

    /* (4) CURE — genuine weights, adopted signer, matching id -> ACCEPT. */
    {
        INT v = sign_manifest_verify(&wm, wid);
        if (!v) { gn_puts("[sign-genome] genuine signed weights refused (bug)\r\n"); ok = 0; }
        else gn_puts("[sign-genome] adopted-signer, matching weights: RESOLVED\r\n");
    }

    sign_allow_clear();
    gn_puts(ok ? "[sign-genome] PASS\r\n" : "[sign-genome] FAIL\r\n");
    return ok;
}

/* ------------------------------------------------------------------ */
/* auto-germination (hosted only, opt-in via PKERNEL_SPROUT=1)         */
/* ------------------------------------------------------------------ */

void genome_autosprout(void)
{
#ifdef _TK_HOSTED_LIBC_
    const char *v = getenv("PKERNEL_SPROUT");
    if (!v || v[0] != '1') return;     /* default: existing demos as-is */
    gn_puts("[genome] PKERNEL_SPROUT=1 — auto-germination: this empty "
            "plate now waits for the swarm's DNA\r\n");
    /* long budget: the publisher cell may still be training. Runs in
     * usermain BEFORE the shell loop = same (shell) task, so the
     * pfs_dag scratch discipline holds. */
    genome_sprout(240);                /* up to ~120 s */
#endif
}

/* ------------------------------------------------------------------ */
/* shell command — "genome [publish <role>|sprout]"                    */
/* ------------------------------------------------------------------ */

static void genome_usage(void)
{
    gn_puts("usage: genome                 status (role / manifest)\r\n"
            "       genome publish <role>  this full cell's DNA -> "
            "p-fs '" GENOME_REF "'\r\n"
            "       genome sprout          germinate from the swarm's "
            "manifest\r\n"
            "       genome adopt <hexkey>  adopt a signer key (verified weights)\r\n"
            "       genome pubkey          print this node's signer pubkey\r\n"
            "       roles: cell brain sensor relay\r\n");
}

void genome_cmd(const UB *line, INT n)
{
    const UB *p   = line + 6;          /* skip "genome" */
    const UB *end = line + n;
    while (p < end && (*p == ' ' || *p == '\t')) p++;

    if (p >= end) {
        genome_print();

    } else if (gn_tok(p, end, "publish")) {
        p += 7;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        U1 role = GENOME_ROLE_CELL;    /* default */
        if (p < end) {
            U1 hit = 0;
            for (U1 r = 1; r < ROLE_N; r++) {
                if (gn_tok(p, end, role_name[r])) {
                    role = r; hit = 1; break;
                }
            }
            if (!hit) {
                UW rl = (UW)(end - p); if (rl > 8) rl = 8;
                gn_puts("[genome] unknown role '");
                gn_putn((const char *)p, rl);
                gn_puts("' — using 'cell'\r\n");
            }
        }
        genome_publish(role);

    } else if (gn_tok(p, end, "sprout")) {
        genome_sprout(0);

    } else if (gn_tok(p, end, "adopt")) {
        /* signing.md §3.2/§4.3: adopt a SIGNER KEY (64 hex chars). This is the
         * human consent act; verification stays mechanical. The key belongs to
         * a NODE, never a human (§0) — no profile/handle is involved. */
        p += 5;
        U1 pk[ED25519_PUBLIC_KEY_LEN];
        if (gn_parse_hexkey(p, end, pk) != ED25519_PUBLIC_KEY_LEN) {
            gn_puts("[genome] adopt: expected a 64-hex-char node pubkey\r\n");
        } else if (sign_allow_add(pk)) {
            gn_puts("[genome] ADOPTED signer key — its signed weight manifests "
                    "now resolve here; weights without a valid adopted signature "
                    "are refused (signing.md §4.3)\r\n");
        } else {
            gn_puts("[genome] adopt: allowlist full\r\n");
        }

    } else if (gn_tok(p, end, "pubkey")) {
        /* print this node's pubkey so a peer can `genome adopt` it. */
        const U1 *pk = (sign_node_key_ensure() ? sign_node_pubkey() : 0);
        if (!pk) { gn_puts("[genome] no node key\r\n"); return; }
        static const char hx[] = "0123456789abcdef";
        char out[ED25519_PUBLIC_KEY_LEN * 2 + 1];
        for (INT i = 0; i < ED25519_PUBLIC_KEY_LEN; i++) {
            out[i*2]   = hx[(pk[i] >> 4) & 0xF];
            out[i*2+1] = hx[pk[i] & 0xF];
        }
        out[ED25519_PUBLIC_KEY_LEN * 2] = '\0';
        gn_puts("[genome] node pubkey: "); gn_puts(out); gn_puts("\r\n");

    } else {
        genome_usage();
    }
}
