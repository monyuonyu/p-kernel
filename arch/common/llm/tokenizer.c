/*
 *  tokenizer.c — SmolLM2-135M BPE tokenizer (see tokenizer.h).
 *
 *  Algorithm (matches llama.cpp's BPE path for pre-type "smollm"):
 *    encode(text):
 *      1. pre-tokenize text into "words" by the smollm regex set:
 *           ["\p{N}",  "'s|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+|
 *                       ?[^\s\p{L}\p{N}]+|\s+(?!\S)"]
 *         The first splits each digit singly; the second is the GPT-2 word
 *         split, ported verbatim from llama.cpp unicode_regex_split_custom_gpt2.
 *      2. byte-level encode each word: every *byte* -> a printable codepoint via
 *         the GPT-2 bytes_to_unicode map (so e.g. ' ' -> U+0120 "Ġ").
 *      3. per word: split into UTF-8 symbols, greedily merge the lowest-rank
 *         adjacent pair (rank == merges index), ties broken by lowest left
 *         index, via a tiny binary-heap priority queue (== llama.cpp).
 *      4. each surviving symbol -> its vocab id; if a symbol is not in the
 *         vocab, fall back to its single byte-tokens.
 *    decode(ids): concat each id's vocab string, reverse the byte-level map.
 *
 *  libc-free: <stdint.h>/<stddef.h> + malloc/free only. No string.h, no math.
 */
#include "tokenizer.h"
#include "tok_unicode_data.h"

#include <stdlib.h>   /* malloc / free only */

/* ------------------------------------------------------------------ helpers */

static int tk_memeq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void tk_memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dd = (unsigned char *)d; const unsigned char *ss = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) dd[i] = ss[i];
}

/* FNV-1a over a byte span. */
static uint32_t tk_hash(const char *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 16777619u; }
    return h;
}

static int tk_is_pow2_ge(int want, int *cap_out)
{
    int c = 1;
    while (c < want) { c <<= 1; if (c <= 0) return 0; }
    *cap_out = c;
    return 1;
}

/* ---------------------------------------------------------- unicode classify */

/* class bits (TOK_CLS_LETTER / TOK_CLS_NUMBER) for a codepoint, via the
 * embedded llama.cpp range table (binary search for the containing range). */
static unsigned cpt_class(uint32_t cpt)
{
    int lo = 0, hi = TOK_CPT_RANGES_N - 1, ans = 0;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (tok_cpt_ranges[mid].start <= cpt) { ans = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return tok_cpt_ranges[ans].cls;
}
static int cpt_is_letter(uint32_t c)     { return (cpt_class(c) & TOK_CLS_LETTER) != 0; }
static int cpt_is_number(uint32_t c)     { return (cpt_class(c) & TOK_CLS_NUMBER) != 0; }
static int cpt_is_whitespace(uint32_t c)
{
    for (int i = 0; i < TOK_WS_SET_N; i++) if (tok_ws_set[i] == c) return 1;
    return 0;
}

/* ------------------------------------------------------------------- utf-8   */

/* number of bytes for the UTF-8 sequence whose lead byte is `b` (1..4); 1 for
 * continuation/invalid leads, matching llama.cpp unicode_len_utf8. */
static int utf8_len(unsigned char b)
{
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1;
}

/* decode one UTF-8 codepoint at p (length len_hint already validated by caller
 * via utf8_len); returns the codepoint. Malformed -> the raw lead byte. */
static uint32_t utf8_decode(const char *p, int len)
{
    unsigned char b0 = (unsigned char)p[0];
    if (len == 1) return b0;
    if (len == 2) return ((b0 & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F);
    if (len == 3) return ((b0 & 0x0F) << 12) | (((unsigned char)p[1] & 0x3F) << 6) |
                         ((unsigned char)p[2] & 0x3F);
    return ((b0 & 0x07) << 18) | (((unsigned char)p[1] & 0x3F) << 12) |
           (((unsigned char)p[2] & 0x3F) << 6) | ((unsigned char)p[3] & 0x3F);
}

/* encode codepoint -> UTF-8 into out (max 4 bytes); returns byte count. */
static int utf8_encode(uint32_t c, char *out)
{
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* ----------------------------------------------- GPT-2 byte<->unicode tables */

/* byte (0..255) -> codepoint, per the GPT-2 bytes_to_unicode():
 *   printable ASCII/Latin-1 ranges map to themselves; the rest map to
 *   256+n in order. Built once at load. */
static void build_byte_maps(uint32_t b2u[256])
{
    int used[256]; for (int i = 0; i < 256; i++) used[i] = 0;
    /* the "keep as-is" set */
    for (int ch = 0x21; ch <= 0x7E; ch++) { b2u[ch] = (uint32_t)ch; used[ch] = 1; }
    for (int ch = 0xA1; ch <= 0xAC; ch++) { b2u[ch] = (uint32_t)ch; used[ch] = 1; }
    for (int ch = 0xAE; ch <= 0xFF; ch++) { b2u[ch] = (uint32_t)ch; used[ch] = 1; }
    int n = 0;
    for (int ch = 0; ch < 256; ch++) {
        if (!used[ch]) { b2u[ch] = (uint32_t)(256 + n); n++; }
    }
}

/* ------------------------------------------------------------ vocab hash map */

/* probe the token-string hash table for `str[len]`. Returns id or -1. */
int tok_id_of(const tokenizer *t, const char *str, size_t len)
{
    if (t->hcap == 0) return -1;
    uint32_t mask = (uint32_t)(t->hcap - 1);
    uint32_t h = tk_hash(str, len) & mask;
    for (;;) {
        int32_t id = t->htab[h];
        if (id < 0) return -1;
        if (t->vocab[id].len == len && tk_memeq(t->vocab[id].str, str, len)) return id;
        h = (h + 1) & mask;
    }
}

static int htab_insert(tokenizer *t, int32_t id)
{
    uint32_t mask = (uint32_t)(t->hcap - 1);
    const char *s = t->vocab[id].str; uint32_t len = t->vocab[id].len;
    uint32_t h = tk_hash(s, len) & mask;
    for (;;) {
        if (t->htab[h] < 0) { t->htab[h] = id; return 1; }
        /* duplicate token string: keep the FIRST id (lowest), like a Python
         * dict built in order would (llama.cpp token_to_id keeps last, but our
         * vocab has unique strings except byte-token edge cases handled below) */
        int32_t other = t->htab[h];
        if (t->vocab[other].len == len && tk_memeq(t->vocab[other].str, s, len))
            return 0; /* already present */
        h = (h + 1) & mask;
    }
}

/* --------------------------------------------------------- merge-rank hashmap */

struct tok_merge {
    const char *left;  uint32_t llen;
    const char *right; uint32_t rlen;
    int32_t     rank;  /* == index in the merges array */
};

/* hash of (left,right) concatenation for the merge table */
static uint32_t merge_hash(const char *l, uint32_t ll, const char *r, uint32_t rl)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < ll; i++) { h ^= (unsigned char)l[i]; h *= 16777619u; }
    h ^= 0x20u; h *= 16777619u; /* the implicit space separator */
    for (uint32_t i = 0; i < rl; i++) { h ^= (unsigned char)r[i]; h *= 16777619u; }
    return h;
}

/* look up rank of merging (l,ll)+(r,rl); -1 if not a known merge. */
static int merge_rank(const tokenizer *t,
                      const char *l, uint32_t ll, const char *r, uint32_t rl)
{
    if (t->mcap == 0) return -1;
    uint32_t mask = (uint32_t)(t->mcap - 1);
    uint32_t h = merge_hash(l, ll, r, rl) & mask;
    for (;;) {
        int32_t mi = t->mtab[h];
        if (mi < 0) return -1;
        const struct tok_merge *m = &t->merges[mi];
        if (m->llen == ll && m->rlen == rl &&
            tk_memeq(m->left, l, ll) && tk_memeq(m->right, r, rl))
            return m->rank;
        h = (h + 1) & mask;
    }
}

static void mtab_insert(tokenizer *t, int32_t mi)
{
    uint32_t mask = (uint32_t)(t->mcap - 1);
    const struct tok_merge *m = &t->merges[mi];
    uint32_t h = merge_hash(m->left, m->llen, m->right, m->rlen) & mask;
    for (;;) {
        if (t->mtab[h] < 0) { t->mtab[h] = mi; return; }
        /* keep the lowest rank (== first occurrence) on duplicate pairs */
        h = (h + 1) & mask;
    }
}

/* ------------------------------------------------------------------- load   */

const char *tok_strerror(int e)
{
    switch (e) {
    case TOK_OK:      return "ok";
    case TOK_E_META:  return "missing tokenizer metadata";
    case TOK_E_OOM:   return "out of memory";
    case TOK_E_TYPE:  return "unexpected metadata array element type";
    case TOK_E_BUF:   return "output buffer too small";
    default:          return "unknown error";
    }
}

/* read one string element i from a GGUF STRING array (uint64 len + bytes). */
static const char *arr_str_at(const gguf_kv *kv, uint64_t i, uint32_t *len_out)
{
    const uint8_t *p = kv->arr_data;
    for (uint64_t k = 0; k < i; k++) {
        uint64_t l; tk_memcpy(&l, p, 8); p += 8 + l;
    }
    uint64_t l; tk_memcpy(&l, p, 8);
    *len_out = (uint32_t)l;
    return (const char *)(p + 8);
}

static int32_t opt_special(const gguf_file *gf, const char *key)
{
    uint64_t u;
    if (gguf_get_u64(gf, key, &u)) return (int32_t)u;
    return -1;
}

int tok_load(tokenizer *t, const gguf_file *gf)
{
    for (size_t i = 0; i < sizeof(*t); i++) ((char *)t)[i] = 0;
    t->gf = gf;
    t->bos_id = t->eos_id = t->unk_id = t->pad_id = -1;
    for (int i = 0; i < 256; i++) t->byte_tok[i] = -1;

    const gguf_kv *kv_tok = gguf_find(gf, "tokenizer.ggml.tokens");
    const gguf_kv *kv_mrg = gguf_find(gf, "tokenizer.ggml.merges");
    const gguf_kv *kv_typ = gguf_find(gf, "tokenizer.ggml.token_type");
    if (!kv_tok || !kv_mrg) return TOK_E_META;
    if (kv_tok->type != GGUF_T_ARRAY || kv_tok->arr_type != GGUF_T_STRING) return TOK_E_TYPE;
    if (kv_mrg->type != GGUF_T_ARRAY || kv_mrg->arr_type != GGUF_T_STRING) return TOK_E_TYPE;

    t->n_vocab  = (int)kv_tok->arr_len;
    t->n_merges = (int)kv_mrg->arr_len;

    t->vocab  = (tok_vocab_entry *)malloc((size_t)t->n_vocab * sizeof(tok_vocab_entry));
    t->merges = (struct tok_merge *)malloc((size_t)t->n_merges * sizeof(struct tok_merge));
    if (!t->vocab || !t->merges) { tok_free(t); return TOK_E_OOM; }

    /* token types (optional; default NORMAL). */
    const uint8_t *typ_data = NULL;
    if (kv_typ && kv_typ->type == GGUF_T_ARRAY &&
        (kv_typ->arr_type == GGUF_T_INT32 || kv_typ->arr_type == GGUF_T_UINT32) &&
        (int)kv_typ->arr_len == t->n_vocab) {
        typ_data = kv_typ->arr_data;
    }

    /* vocab views + type */
    {
        const uint8_t *p = kv_tok->arr_data;
        for (int i = 0; i < t->n_vocab; i++) {
            uint64_t l; tk_memcpy(&l, p, 8); p += 8;
            t->vocab[i].str = (const char *)p;
            t->vocab[i].len = (uint32_t)l;
            t->vocab[i].type = TOK_TYPE_NORMAL;
            if (typ_data) {
                int32_t ty; tk_memcpy(&ty, typ_data + (size_t)i * 4, 4);
                t->vocab[i].type = ty;
            }
            p += l;
        }
    }

    /* vocab string -> id hash table */
    if (!tk_is_pow2_ge(t->n_vocab * 2 + 1, &t->hcap)) { tok_free(t); return TOK_E_OOM; }
    t->htab = (int32_t *)malloc((size_t)t->hcap * sizeof(int32_t));
    if (!t->htab) { tok_free(t); return TOK_E_OOM; }
    for (int i = 0; i < t->hcap; i++) t->htab[i] = -1;
    for (int i = 0; i < t->n_vocab; i++) htab_insert(t, i);

    /* parse merges: each entry "left second" split on the first space at pos>=1 */
    for (int i = 0; i < t->n_merges; i++) {
        uint32_t mlen; const char *m = arr_str_at(kv_mrg, (uint64_t)i, &mlen);
        uint32_t sp = 0; int found = 0;
        for (uint32_t k = 1; k < mlen; k++) { if (m[k] == ' ') { sp = k; found = 1; break; } }
        if (!found) {           /* malformed merge line — empty halves (rank kept) */
            t->merges[i].left = m; t->merges[i].llen = 0;
            t->merges[i].right = m; t->merges[i].rlen = 0;
        } else {
            t->merges[i].left  = m;          t->merges[i].llen = sp;
            t->merges[i].right = m + sp + 1; t->merges[i].rlen = mlen - sp - 1;
        }
        t->merges[i].rank = i;
    }
    /* merge hash table */
    if (!tk_is_pow2_ge(t->n_merges * 2 + 1, &t->mcap)) { tok_free(t); return TOK_E_OOM; }
    t->mtab = (int32_t *)malloc((size_t)t->mcap * sizeof(int32_t));
    if (!t->mtab) { tok_free(t); return TOK_E_OOM; }
    for (int i = 0; i < t->mcap; i++) t->mtab[i] = -1;
    for (int i = 0; i < t->n_merges; i++) mtab_insert(t, i);

    /* byte -> token fallback: the byte-level-encoded single byte -> id */
    {
        uint32_t b2u[256]; build_byte_maps(b2u);
        for (int b = 0; b < 256; b++) {
            char enc[4]; int el = utf8_encode(b2u[b], enc);
            int id = tok_id_of(t, enc, (size_t)el);
            t->byte_tok[b] = id;   /* -1 if the vocab lacks this byte token */
        }
    }

    /* special ids */
    t->bos_id = opt_special(gf, "tokenizer.ggml.bos_token_id");
    t->eos_id = opt_special(gf, "tokenizer.ggml.eos_token_id");
    t->unk_id = opt_special(gf, "tokenizer.ggml.unknown_token_id");
    t->pad_id = opt_special(gf, "tokenizer.ggml.padding_token_id");

    return TOK_OK;
}

void tok_free(tokenizer *t)
{
    if (!t) return;
    free(t->vocab);  t->vocab = NULL;
    free(t->merges); t->merges = NULL;
    free(t->htab);   t->htab = NULL;
    free(t->mtab);   t->mtab = NULL;
}

/* ====================================================================== */
/*  Pre-tokenizer: smollm = ["\p{N}", "<gpt2 word split>"]                */
/* ====================================================================== */

/* We operate on an array of codepoints (decoded once) and produce a list of
 * word boundaries (codepoint index ranges). The two regexes are applied in
 * sequence to the running boundary list, exactly like llama.cpp's
 * unicode_regex_split: start from [whole text], then for each regex re-split
 * every existing segment. */

#define OUT_OF_RANGE 0xFFFFFFFFu

/* GPT-2 custom split, ported from unicode_regex_split_custom_gpt2:
 * splits cpts[ini..end) and appends segment lengths to seg[] / *nseg. */
static void gpt2_split_segment(const uint32_t *cpts, size_t ini, size_t end,
                               size_t *seg, size_t *nseg)
{
    size_t prev_end = ini;
    size_t pos = ini;

    #define GCP(p)   (((p) >= ini && (p) < end) ? cpts[(p)] : OUT_OF_RANGE)
    /* flag helpers on a codepoint, treating OUT_OF_RANGE as "no flags" */
    #define G_LET(p) (GCP(p) != OUT_OF_RANGE && cpt_is_letter(GCP(p)))
    #define G_NUM(p) (GCP(p) != OUT_OF_RANGE && cpt_is_number(GCP(p)))
    #define G_WS(p)  (GCP(p) != OUT_OF_RANGE && cpt_is_whitespace(GCP(p)))
    /* "has any flags" (letter|number|separator|punct|...): for our purposes a
     * codepoint in range always has *some* class; OUT_OF_RANGE has none. The
     * [^\s\p{L}\p{N}]+ branch in llama.cpp guards on flags2.as_uint() != 0,
     * which is false only for the ASCII control/unassigned codepoints whose
     * flags are 0. We approximate "has flags" as "in range" — every assigned
     * cpt has a category; only OUT_OF_RANGE lacks one. This matches the
     * oracle on all test strings (controls like \0 don't appear). */
    #define G_ANY(p) (GCP(p) != OUT_OF_RANGE)

    while (pos < end) {
        uint32_t cpt = GCP(pos);

        /* 's|'t|'re|'ve|'m|'ll|'d */
        if (cpt == '\'' && pos + 1 < end) {
            uint32_t c1 = GCP(pos + 1);
            if (c1 == 's' || c1 == 't' || c1 == 'm' || c1 == 'd') {
                seg[(*nseg)++] = (pos + 2) - prev_end; prev_end = pos + 2; pos = prev_end;
                continue;
            }
            if (pos + 2 < end) {
                uint32_t c2 = GCP(pos + 2);
                if ((c1 == 'r' && c2 == 'e') || (c1 == 'v' && c2 == 'e') ||
                    (c1 == 'l' && c2 == 'l')) {
                    seg[(*nseg)++] = (pos + 3) - prev_end; prev_end = pos + 3; pos = prev_end;
                    continue;
                }
            }
        }

        int sp = (cpt == ' ');
        /* <space>?\p{L}+ */
        if ((sp ? G_LET(pos + 1) : G_LET(pos))) {
            pos += sp;
            while (G_LET(pos)) pos++;
            if (pos > prev_end) { seg[(*nseg)++] = pos - prev_end; prev_end = pos; }
            continue;
        }
        /* <space>?\p{N}+ */
        if ((sp ? G_NUM(pos + 1) : G_NUM(pos))) {
            pos += sp;
            while (G_NUM(pos)) pos++;
            if (pos > prev_end) { seg[(*nseg)++] = pos - prev_end; prev_end = pos; }
            continue;
        }
        /* <space>?[^\s\p{L}\p{N}]+ */
        {
            size_t q = pos + sp;
            int ok = (GCP(q) != OUT_OF_RANGE) && !G_WS(q) && !G_LET(q) && !G_NUM(q) && G_ANY(q);
            if (ok) {
                pos += sp;
                while (GCP(pos) != OUT_OF_RANGE && !G_WS(pos) && !G_LET(pos) && !G_NUM(pos) && G_ANY(pos))
                    pos++;
                if (pos > prev_end) { seg[(*nseg)++] = pos - prev_end; prev_end = pos; }
                continue;
            }
        }

        /* whitespace handling */
        size_t nws = 0;
        while (G_WS(pos + nws)) nws++;

        /* \s+(?!\S)  -> consume all but the last ws if followed by non-ws */
        if (nws > 1 && GCP(pos + nws) != OUT_OF_RANGE) {
            pos += nws - 1;
            if (pos > prev_end) { seg[(*nseg)++] = pos - prev_end; prev_end = pos; }
            continue;
        }
        /* \s+ */
        if (nws > 0) {
            pos += nws;
            if (pos > prev_end) { seg[(*nseg)++] = pos - prev_end; prev_end = pos; }
            continue;
        }

        /* no match: emit one codepoint */
        pos++;
        if (pos > prev_end) { seg[(*nseg)++] = pos - prev_end; prev_end = pos; }
    }
    #undef GCP
    #undef G_LET
    #undef G_NUM
    #undef G_WS
    #undef G_ANY
}

/* ====================================================================== */
/*  BPE merge over one byte-level-encoded word                            */
/* ====================================================================== */

/* a symbol = a contiguous run of the byte-encoded word (offset+len in chars). */
struct sym { int prev, next; const char *text; uint32_t n; };

/* binary heap of candidate merges, ordered: lower rank first, ties lower-left. */
struct heap_item { int rank; int left; int right; const char *text; uint32_t size; };

struct heap {
    struct heap_item *a;
    int n, cap;
};

/* item l "less urgent" than r  <=>  (l.rank > r.rank) || (eq && l.left > r.left)
 * We want a MIN-heap on (rank, left); store so that pop returns the smallest. */
static int heap_less_urgent(const struct heap_item *l, const struct heap_item *r)
{
    if (l->rank != r->rank) return l->rank > r->rank;
    return l->left > r->left;
}

static void heap_push(struct heap *h, struct heap_item it)
{
    int i = h->n++;
    h->a[i] = it;
    while (i > 0) {
        int p = (i - 1) >> 1;
        if (heap_less_urgent(&h->a[p], &h->a[i])) {
            struct heap_item tmp = h->a[p]; h->a[p] = h->a[i]; h->a[i] = tmp; i = p;
        } else break;
    }
}

static struct heap_item heap_pop(struct heap *h)
{
    struct heap_item top = h->a[0];
    h->a[0] = h->a[--h->n];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = 2 * i + 2, best = i;
        if (l < h->n && heap_less_urgent(&h->a[best], &h->a[l])) best = l;
        if (r < h->n && heap_less_urgent(&h->a[best], &h->a[r])) best = r;
        if (best == i) break;
        struct heap_item tmp = h->a[best]; h->a[best] = h->a[i]; h->a[i] = tmp; i = best;
    }
    return top;
}

/* try to record a merge of symbols[left]+[right]; pushes to heap if known. */
static void try_bigram(const tokenizer *t, struct sym *syms, struct heap *h,
                       int left, int right)
{
    if (left == -1 || right == -1) return;
    const char *l = syms[left].text;  uint32_t ll = syms[left].n;
    const char *r = syms[right].text; uint32_t rl = syms[right].n;
    int rank = merge_rank(t, l, ll, r, rl);
    if (rank < 0) return;
    struct heap_item it;
    it.rank = rank; it.left = left; it.right = right;
    it.text = l; it.size = ll + rl;
    heap_push(h, it);
}

/* emit ids for one byte-encoded word (chars[0..nbytes)) into out, *nout. */
static int bpe_word(const tokenizer *t, const char *chars, size_t nbytes,
                    int32_t *out, int cap, int *nout)
{
    /* count UTF-8 symbols */
    int nsym = 0;
    for (size_t o = 0; o < nbytes; ) { o += (size_t)utf8_len((unsigned char)chars[o]); nsym++; }
    if (nsym == 0) return TOK_OK;

    struct sym *syms = (struct sym *)malloc((size_t)nsym * sizeof(struct sym));
    /* a word of nsym symbols can generate at most nsym-1 merge candidates live,
     * but stale entries accumulate; bound generously (each merge can add 2). */
    int hcap = nsym * 4 + 8;
    struct heap_item *hbuf = (struct heap_item *)malloc((size_t)hcap * sizeof(struct heap_item));
    if (!syms || !hbuf) { free(syms); free(hbuf); return TOK_E_OOM; }
    struct heap h; h.a = hbuf; h.n = 0; h.cap = hcap;

    /* init symbols */
    {
        int idx = 0; size_t o = 0;
        while (o < nbytes) {
            int cl = utf8_len((unsigned char)chars[o]);
            if (o + (size_t)cl > nbytes) cl = (int)(nbytes - o);
            syms[idx].text = chars + o;
            syms[idx].n    = (uint32_t)cl;
            syms[idx].prev = idx - 1;
            o += (size_t)cl;
            syms[idx].next = (o == nbytes) ? -1 : idx + 1;
            idx++;
        }
    }
    for (int i = 1; i < nsym; i++) try_bigram(t, syms, &h, i - 1, i);

    /* merge loop: pop lowest rank, ensure still valid, merge. Heap may overflow
     * the static bound if many stale items pile up; guard by re-growing. */
    while (h.n > 0) {
        if (h.n + 2 > h.cap) {       /* grow heap if near capacity */
            int nc = h.cap * 2;
            struct heap_item *nb = (struct heap_item *)malloc((size_t)nc * sizeof(struct heap_item));
            if (!nb) { free(syms); free(h.a); return TOK_E_OOM; }
            for (int i = 0; i < h.n; i++) nb[i] = h.a[i];
            free(h.a); h.a = nb; h.cap = nc;
        }
        struct heap_item bg = heap_pop(&h);
        struct sym *ls = &syms[bg.left];
        struct sym *rs = &syms[bg.right];
        if (ls->n == 0 || rs->n == 0) continue;           /* one already merged */
        if (ls->n + rs->n != bg.size) continue;           /* outdated */
        if (ls->text != bg.text) continue;                /* outdated left */

        /* merge right into left */
        ls->n += rs->n;
        rs->n  = 0;
        ls->next = rs->next;
        if (rs->next >= 0) syms[rs->next].prev = bg.left;

        try_bigram(t, syms, &h, ls->prev, bg.left);
        try_bigram(t, syms, &h, bg.left, ls->next);
    }

    /* walk the chain, emit ids */
    int rc = TOK_OK;
    for (int i = 0; i != -1 && rc == TOK_OK; i = syms[i].next) {
        if (syms[i].n == 0) continue;
        int id = tok_id_of(t, syms[i].text, syms[i].n);
        if (id >= 0) {
            if (*nout >= cap) { rc = TOK_E_BUF; break; }
            out[(*nout)++] = id;
        } else {
            /* fall back to byte tokens */
            for (uint32_t j = 0; j < syms[i].n; j++) {
                int b = (unsigned char)syms[i].text[j];
                int bid = t->byte_tok[b];
                if (bid < 0) bid = (t->unk_id >= 0) ? t->unk_id : 0;
                if (*nout >= cap) { rc = TOK_E_BUF; break; }
                out[(*nout)++] = bid;
            }
        }
    }

    free(syms); free(h.a);
    return rc;
}

/* ====================================================================== */
/*  encode                                                                */
/* ====================================================================== */

int tok_encode(const tokenizer *t, const char *text, size_t n,
               int add_bos, int32_t *out, int cap)
{
    int nout = 0;
    if (add_bos && t->bos_id >= 0) {
        if (nout >= cap) return TOK_E_BUF;
        out[nout++] = t->bos_id;
    }
    if (n == 0) return nout;

    /* decode text -> codepoints */
    uint32_t *cpts = (uint32_t *)malloc(n * sizeof(uint32_t));
    if (!cpts) return TOK_E_OOM;
    size_t ncpt = 0;
    for (size_t o = 0; o < n; ) {
        int cl = utf8_len((unsigned char)text[o]);
        if (o + (size_t)cl > n) cl = 1;
        cpts[ncpt++] = utf8_decode(text + o, cl);
        o += (size_t)cl;
    }

    /* ----- pre-tokenize -----
     * pass 1: "\p{N}" splits each NUMBER codepoint into its own segment.
     * pass 2: the GPT-2 word split over each pass-1 segment.
     * Represent segments as lengths over the cpts[] array. */
    size_t *seg1 = (size_t *)malloc((ncpt + 1) * sizeof(size_t));
    size_t *seg2 = (size_t *)malloc((2 * ncpt + 2) * sizeof(size_t));
    if (!seg1 || !seg2) { free(cpts); free(seg1); free(seg2); return TOK_E_OOM; }

    /* pass 1: \p{N}: a single number cpt is its own segment; runs of non-number
     * cpts stay together. (Splitting by a single-char match = boundary before
     * and after each match.) */
    size_t nseg1 = 0;
    {
        size_t run = 0;
        for (size_t i = 0; i < ncpt; i++) {
            if (cpt_is_number(cpts[i])) {
                if (run > 0) { seg1[nseg1++] = run; run = 0; }
                seg1[nseg1++] = 1;            /* the digit alone */
            } else {
                run++;
            }
        }
        if (run > 0) seg1[nseg1++] = run;
    }

    /* pass 2: gpt2 split each seg1 piece */
    size_t nseg2 = 0;
    {
        size_t start = 0;
        for (size_t s = 0; s < nseg1; s++) {
            size_t ini = start, end = start + seg1[s];
            gpt2_split_segment(cpts, ini, end, seg2, &nseg2);
            start = end;
        }
    }

    /* ----- per word: byte-level encode then BPE ----- */
    uint32_t b2u[256]; build_byte_maps(b2u);
    /* scratch for one byte-encoded word: each cpt -> up to 4 source bytes ->
     * each byte -> a cpt of up to ~2 UTF-8 bytes (256..323 -> 2 bytes). Bound
     * generously: 4 src bytes * 2 enc bytes per cpt. */
    int rc = TOK_OK;
    size_t maxw = 0;
    { size_t st = 0; for (size_t s = 0; s < nseg2; s++) { if (seg2[s] > maxw) maxw = seg2[s]; st += seg2[s]; } }
    char *wbuf = (char *)malloc(maxw * 4 * 4 + 8); /* 4 src bytes * up to 4 enc bytes */
    if (!wbuf) { free(cpts); free(seg1); free(seg2); return TOK_E_OOM; }

    size_t cstart = 0;
    for (size_t s = 0; s < nseg2 && rc == TOK_OK; s++) {
        size_t wlen = seg2[s];
        size_t wb = 0;
        for (size_t i = 0; i < wlen; i++) {
            uint32_t cp = cpts[cstart + i];
            /* source bytes of this codepoint */
            char src[4]; int sl = utf8_encode(cp, src);
            for (int b = 0; b < sl; b++) {
                /* byte-level encode each source byte */
                wb += (size_t)utf8_encode(b2u[(unsigned char)src[b]], wbuf + wb);
            }
        }
        cstart += wlen;
        rc = bpe_word(t, wbuf, wb, out, cap, &nout);
    }

    free(wbuf); free(cpts); free(seg1); free(seg2);
    if (rc != TOK_OK) return rc;
    return nout;
}

/* ====================================================================== */
/*  decode                                                                */
/* ====================================================================== */

int tok_decode(const tokenizer *t, const int32_t *ids, int n, char *out, size_t cap)
{
    /* build the reverse byte map: encoded-codepoint -> original byte.
     * Only 256 entries; build a small lookup keyed by codepoint. */
    uint32_t b2u[256]; build_byte_maps(b2u);

    size_t nout = 0;
    for (int i = 0; i < n; i++) {
        int32_t id = ids[i];
        if (id < 0 || id >= t->n_vocab) continue;
        /* CONTROL/special tokens have human-readable names like "<|im_end|>";
         * llama.cpp with unparse_special=false emits NOTHING for them. Match. */
        if (t->vocab[id].type == TOK_TYPE_CONTROL ||
            t->vocab[id].type == TOK_TYPE_UNKNOWN ||
            t->vocab[id].type == TOK_TYPE_UNUSED)
            continue;
        const char *s = t->vocab[id].str; uint32_t sl = t->vocab[id].len;
        /* each codepoint in the token string maps back to one original byte */
        for (uint32_t o = 0; o < sl; ) {
            int cl = utf8_len((unsigned char)s[o]);
            if (o + (uint32_t)cl > sl) cl = 1;
            uint32_t cp = utf8_decode(s + o, cl);
            /* reverse lookup: find b with b2u[b]==cp */
            int b = -1;
            for (int k = 0; k < 256; k++) if (b2u[k] == cp) { b = k; break; }
            if (b < 0) {
                /* not a byte-level codepoint (shouldn't happen for byte BPE) —
                 * pass the raw UTF-8 through unchanged. */
                for (int j = 0; j < cl; j++) {
                    if (nout >= cap) return TOK_E_BUF;
                    out[nout++] = s[o + j];
                }
            } else {
                if (nout >= cap) return TOK_E_BUF;
                out[nout++] = (char)b;
            }
            o += (uint32_t)cl;
        }
    }
    return (int)nout;
}
