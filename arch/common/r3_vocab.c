/* ------------------------------------------------------------------ *
 *  r3_vocab.c — LM-8 (living-mind.md Part IX): the fixed, embedded,
 *  content-addressed word list. Real WORDS in, one real WORD out.
 *
 *  WORD LIST v2 (LM-9 Part X): ENGLISH, 16 key words + 64 answer words
 *  (the LM-8 8/32 kept as a prefix — see vk_img/vv_img below).
 *
 *  HONEST DIMS (measured, not the 256/64 the design hoped — IX.0 #2): the
 *  R_DM=32 substrate's in-context KEY recall caps near 8 keys, and the
 *  ANSWER classifier caps near 32 classes (both MEASURED: K>=12 or V>=64
 *  collapse base recall to below the best fixed rule — see the commit
 *  message + the [lang-capacity] curve). So v1 ships the LARGEST
 *  honestly-LEARNABLE real-word vocab at R_DM=32: 8 nameable key words,
 *  32 one-word answers. Widening to 256/64 needs R_DM=48 (a shared-kernel
 *  DTR_LN_MAXW bump) — NAMED as the measured follow-up the cert mandates
 *  (COMMANDER DECISION 5), NOT guessed here. Multilingual = a further
 *  follow-up; the manifesto stays 32-language (ark-profile unchanged) —
 *  only the toy *vocabulary* is English v1. A non-English typer gets an
 *  HONEST REFUSAL (r3_vocab_*_id returns -1), never a mojibake binding.
 *
 *  Each list is ONE newline-separated UTF-8 byte image; token id = the
 *  word's 0-based line index. The image's content-id is pfs_id_compute
 *  (THE one hash) — served to the UI by GET /vocab so the page and the
 *  kernel PROVABLY share the same list (a mismatch is a detectable id
 *  disagreement, not a silent fake binding, IX.3). NO hashing of input
 *  words, NO BPE, NO OOV collisions (IX.0 #6).
 * ------------------------------------------------------------------ */
#include "r3_vocab.h"

/* ---- the two embedded word images (newline-separated, v1 English) -- */
static const char vk_img[] =
    /* LM-9 Part X: 8->16 key words. The original 8 stay as a PREFIX (ids 0..7
     * unchanged) so /vocab content-id change is HONEST; ids 8..15 are new. */
    "sky\nsea\nsun\ngrass\nblood\nnight\ngold\nsnow\n"
    "fire\nleaf\nstone\nwater\ncloud\nrose\nbone\nink\n"
    ;

static const char vv_img[] =
    /* LM-9 Part X: 32->64 answer words. The original 32 stay as a PREFIX
     * (ids 0..31 unchanged) so /vocab content-id change is HONEST. */
    "blue\ngreen\nred\nyellow\nblack\nwhite\ngold\nsilver\n"
    "warm\ncold\nbright\ndark\ndeep\nhigh\nclear\ncalm\n"
    "soft\nhard\nwet\ndry\nbig\nsmall\nfast\nslow\n"
    "near\nfar\nnorth\nsouth\neast\nwest\nup\ndown\n"
    "heavy\nlight\nthick\nthin\nrough\nsmooth\nsharp\nblunt\n"
    "loud\nquiet\nfull\nempty\nopen\nshut\nrich\npoor\n"
    "young\nold\nfresh\nstale\nsweet\nbitter\nsour\nsalty\n"
    "left\nright\nfront\nback\ninner\nouter\nfirst\nlast\n"
    ;

/* ---- content-ids of the EXACT bytes (computed once, lazily) -------- */
static U1  vk_id[PFS_ID_LEN];
static U1  vv_id[PFS_ID_LEN];
static UB  v_id_ready = 0;

static UW v_strlen(const char *s) { UW n = 0; while (s[n]) n++; return n; }

static void v_ensure_ids(void)
{
    if (v_id_ready) return;
    pfs_id_compute(vk_img, v_strlen(vk_img), vk_id);
    pfs_id_compute(vv_img, v_strlen(vv_img), vv_id);
    v_id_ready = 1;
}

/* ---- word <-> id over a newline-separated image -------------------- *
 *  Linear scan: the lists are small, bounded, cold-path (mind teach/ask
 *  are human-paced, IX.3). Returns the 0-based line index, or -1 (OOV). */
static INT v_id(const char *img, const char *w, UW wlen)
{
    if (!w) return -1;
    if (wlen == 0) wlen = v_strlen(w);
    if (wlen == 0) return -1;
    INT idx = 0;
    const char *p = img;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        UW llen = (UW)(e - p);
        if (llen == wlen) {
            UW i = 0;
            while (i < wlen && p[i] == w[i]) i++;
            if (i == wlen) return idx;
        }
        idx++;
        p = (*e == '\n') ? e + 1 : e;
    }
    return -1;
}

static const char *v_word(const char *img, INT id, char *buf, UW bufmax)
{
    if (id < 0) return (const char *)0;
    INT idx = 0;
    const char *p = img;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        if (idx == id) {
            UW llen = (UW)(e - p);
            if (llen >= bufmax) llen = bufmax - 1;
            for (UW i = 0; i < llen; i++) buf[i] = p[i];
            buf[llen] = 0;
            return buf;
        }
        idx++;
        p = (*e == '\n') ? e + 1 : e;
    }
    return (const char *)0;
}

static char vk_wbuf[32];
static char vv_wbuf[32];

INT r3_vocab_key_id(const char *w, UW len) { return v_id(vk_img, w, len); }
INT r3_vocab_val_id(const char *w, UW len) { return v_id(vv_img, w, len); }

const char *r3_vocab_key_word(INT id)
{
    if (id < 0 || id >= (INT)R3_VOCAB_KEYS) return (const char *)0;
    return v_word(vk_img, id, vk_wbuf, sizeof vk_wbuf);
}
const char *r3_vocab_val_word(INT id)
{
    if (id < 0 || id >= (INT)R3_VOCAB_VALS) return (const char *)0;
    return v_word(vv_img, id, vv_wbuf, sizeof vv_wbuf);
}

UW r3_vocab_key_count(void) { return (UW)R3_VOCAB_KEYS; }
UW r3_vocab_val_count(void) { return (UW)R3_VOCAB_VALS; }

void r3_vocab_key_id_blob(U1 out[PFS_ID_LEN])
{ v_ensure_ids(); for (UW i = 0; i < PFS_ID_LEN; i++) out[i] = vk_id[i]; }
void r3_vocab_val_id_blob(U1 out[PFS_ID_LEN])
{ v_ensure_ids(); for (UW i = 0; i < PFS_ID_LEN; i++) out[i] = vv_id[i]; }

const char *r3_vocab_key_blob(UW *len_out)
{ if (len_out) *len_out = v_strlen(vk_img); return vk_img; }
const char *r3_vocab_val_blob(UW *len_out)
{ if (len_out) *len_out = v_strlen(vv_img); return vv_img; }
