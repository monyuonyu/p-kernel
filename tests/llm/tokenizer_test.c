/*
 *  tokenizer_test.c — host cert for the M1d BPE tokenizer
 *                     (arch/common/llm/tokenizer.c).
 *
 *  Three layers (mirroring run_tokenizer.sh):
 *
 *  (1) SELF-CONTAINED SANITY UNIT  [tok-sanity]  (no network, no model file):
 *      build a TINY but spec-valid GGUF in memory with a hand-built byte-level
 *      BPE vocab + merges, then check encode()/decode() against KNOWN ids.
 *      Exit 0 = PASS. This is the "passes with no oracle" cert.
 *
 *  (2) REAL-MODEL ENCODE/DECODE  (needs the SmolLM2-135M GGUF, arg 1):
 *      load the real model's tokenizer and, for a FIXED set of strings, print
 *      machine-parseable "ENC[i]: ..." (token ids) and a roundtrip verdict
 *      "RT[i]: OK/FAIL". run_tokenizer.sh runs the llama.cpp oracle on the SAME
 *      strings and diffs the ENC ids token-for-token ([tok-encode]); the RT
 *      lines are the byte-exact decode(encode(s))==s cert ([tok-roundtrip]).
 *
 *  (3) END-TO-END (optional, if the SmolLM2 GGUF is present): feed
 *      encode("The capital of France is") to the M1c forward and confirm we get
 *      the same prompt ids the M1c cert used (504 3575 282 4649 314).
 *
 *  Build (wave-49): -O1 -ffp-contract=off (one math everywhere).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../arch/common/llm/gguf.h"
#include "../../arch/common/llm/tokenizer.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

/* ---------- little-endian byte writer (same shape as forward_test.c) ----- */
typedef struct { uint8_t *p; size_t cap, len; } buf;
static void bput(buf *b, const void *src, size_t n) {
    if (b->len + n > b->cap) { b->cap = (b->len + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->len, src, n); b->len += n;
}
static void bu32(buf *b, uint32_t v) { uint8_t t[4]; for(int i=0;i<4;i++)t[i]=(v>>(8*i))&0xff; bput(b,t,4); }
static void bu64(buf *b, uint64_t v) { uint8_t t[8]; for(int i=0;i<8;i++)t[i]=(v>>(8*i))&0xff; bput(b,t,8); }
static void bstr(buf *b, const char *s){ uint64_t n=strlen(s); bu64(b,n); bput(b,(const void*)s,n); }
static void bstrn(buf *b, const char *s, size_t n){ bu64(b,n); bput(b,(const void*)s,n); }
static void kv_u32(buf *b, const char *k, uint32_t v){ bstr(b,k); bu32(b,GGUF_T_UINT32); bu32(b,v); }

/* ============== synthetic tiny tokenizer: vocab + merges ================= */
/*  A minimal byte-level BPE: vocab has the single bytes we need plus a few
 *  merged tokens. We byte-level-encode (GPT-2 map) so ' ' -> "Ġ" (U+0120).
 *  Tokens (id):
 *     0  "<unk>"   (UNKNOWN)
 *     1  "a"
 *     2  "b"
 *     3  "Ġ"       (the byte-encoded space, U+0120)
 *     4  "ab"      (merge of a+b, rank 0)
 *     5  "Ġa"      (merge of Ġ+a, rank 1)
 *     6  "Ġab"     (merge of Ġa + b, rank 2)
 *  merges: "a b" (0), "Ġ a" (1), "Ġa b" (2)
 *  So:  "ab"   -> [4]
 *       "a b"  -> [1, 5]      (the second "b" has a leading space "Ġb"? no:
 *                              " b" word -> "Ġb"; Ġb not in vocab nor a merge,
 *                              so -> bytes Ġ(3) b(2))  => [1, 3, 2]
 *       "ab ab"-> [4, 3, 4]   (second word " ab" -> "Ġab": the a+b merge has
 *                              rank 0 < the Ġ+a merge rank 1, so "a"+"b" merges
 *                              FIRST -> Ġ,ab; "Ġ ab" is not a merge -> Ġ(3) ab(4).
 *                              This is exactly GPT-2 greedy-by-rank behaviour.)
 */
static const char *TOK_STR[] = { "<unk>", "a", "b", "\xC4\xA0", "ab",
                                 "\xC4\xA0""a", "\xC4\xA0""ab" };
static const int   TOK_TYP[] = { TOK_TYPE_UNKNOWN, 1,1,1,1,1,1 };
#define NTOK 7
static const char *MERGES[] = { "a b", "\xC4\xA0 a", "\xC4\xA0""a b" };
#define NMERGE 3

static int build_tiny_gguf(const char *path) {
    buf b = {0,0,0};
    /* header */
    bput(&b, "GGUF", 4);
    bu32(&b, 3);            /* version */
    bu64(&b, 0);           /* tensor_count */
    /* kv count: tokens, token_type, merges, model, bos, eos, unk = 7 */
    bu64(&b, 7);

    /* tokenizer.ggml.model = "gpt2" */
    bstr(&b, "tokenizer.ggml.model"); bu32(&b, GGUF_T_STRING); bstr(&b, "gpt2");

    /* tokenizer.ggml.tokens : ARRAY<STRING> */
    bstr(&b, "tokenizer.ggml.tokens"); bu32(&b, GGUF_T_ARRAY);
    bu32(&b, GGUF_T_STRING); bu64(&b, NTOK);
    for (int i = 0; i < NTOK; i++) bstr(&b, TOK_STR[i]);

    /* tokenizer.ggml.token_type : ARRAY<INT32> */
    bstr(&b, "tokenizer.ggml.token_type"); bu32(&b, GGUF_T_ARRAY);
    bu32(&b, GGUF_T_INT32); bu64(&b, NTOK);
    for (int i = 0; i < NTOK; i++) bu32(&b, (uint32_t)TOK_TYP[i]);

    /* tokenizer.ggml.merges : ARRAY<STRING> */
    bstr(&b, "tokenizer.ggml.merges"); bu32(&b, GGUF_T_ARRAY);
    bu32(&b, GGUF_T_STRING); bu64(&b, NMERGE);
    for (int i = 0; i < NMERGE; i++) bstr(&b, MERGES[i]);

    /* specials */
    kv_u32(&b, "tokenizer.ggml.bos_token_id", 5);
    kv_u32(&b, "tokenizer.ggml.eos_token_id", 6);
    kv_u32(&b, "tokenizer.ggml.unknown_token_id", 0);

    /* pad to 32 (general.alignment default) so data_offset is valid even with
     * zero tensors (gguf_open computes data_offset from current pos). */
    while (b.len % 32) { uint8_t z = 0; bput(&b, &z, 1); }

    FILE *f = fopen(path, "wb");
    if (!f) { free(b.p); return -1; }
    fwrite(b.p, 1, b.len, f); fclose(f); free(b.p);
    (void)bstrn;
    return 0;
}

static int ids_eq(const int32_t *a, int na, const int *exp, int ne) {
    if (na != ne) return 0;
    for (int i = 0; i < na; i++) if (a[i] != exp[i]) return 0;
    return 1;
}

static int sanity_unit(void) {
    printf("\n=== [tok-sanity] TINY HAND-BUILT BPE CERT (no network) ===\n");
    char path[] = "/tmp/tok_tiny_XXXXXX";
    int fd = mkstemp(path); if (fd >= 0) close(fd);
    if (build_tiny_gguf(path) != 0) { printf("  FAIL build_tiny_gguf\n"); return 1; }

    gguf_file gf;
    int rc = gguf_open(&gf, path);
    if (rc != GGUF_OK) { printf("  FAIL gguf_open: %s\n", gguf_strerror(rc)); unlink(path); return 1; }

    tokenizer t;
    rc = tok_load(&t, &gf);
    if (rc != TOK_OK) { printf("  FAIL tok_load: %s\n", tok_strerror(rc)); gguf_close(&gf); unlink(path); return 1; }
    CHECK(t.n_vocab == NTOK, "n_vocab == 7");
    CHECK(t.n_merges == NMERGE, "n_merges == 3");
    CHECK(t.bos_id == 5 && t.eos_id == 6 && t.unk_id == 0, "special ids loaded");

    int32_t out[64];
    struct { const char *s; int exp[8]; int ne; } cases[] = {
        { "ab",     { 4 },          1 },     /* a+b merge */
        { "a",      { 1 },          1 },
        { "b",      { 2 },          1 },
        { "ab ab",  { 4, 3, 4 },    3 },     /* "ab" + " ab"->Ġ,ab (rank order) */
        { "a b",    { 1, 3, 2 },    3 },     /* "a" + word " b"->"Ġb"-> Ġ b */
    };
    for (int c = 0; c < (int)(sizeof(cases)/sizeof(cases[0])); c++) {
        int n = tok_encode(&t, cases[c].s, strlen(cases[c].s), 0, out, 64);
        char msg[128];
        snprintf(msg, sizeof msg, "encode(\"%s\") == expected", cases[c].s);
        int ok = (n >= 0) && ids_eq(out, n, cases[c].exp, cases[c].ne);
        if (!ok) { printf("    got:"); for (int i=0;i<n;i++) printf(" %d", out[i]); printf("\n"); }
        CHECK(ok, msg);
        /* roundtrip */
        if (n >= 0) {
            char dec[128]; int dn = tok_decode(&t, out, n, dec, sizeof dec);
            int rt = (dn == (int)strlen(cases[c].s)) && memcmp(dec, cases[c].s, dn) == 0;
            snprintf(msg, sizeof msg, "decode(encode(\"%s\")) == input", cases[c].s);
            CHECK(rt, msg);
        }
    }

    /* add_bos prepends bos_id */
    {
        int n = tok_encode(&t, "ab", 2, /*add_bos=*/1, out, 64);
        CHECK(n == 2 && out[0] == 5 && out[1] == 4, "add_bos prepends bos_id");
    }

    tok_free(&t);
    gguf_close(&gf);
    unlink(path);
    return 0;
}

/* ============== real-model strings (must match the oracle's list) ======== */
static const char *STRINGS[] = {
    "The capital of France is",
    "Hello, world!",
    "hello world",
    " leading space",
    "trailing space ",
    "newlines\nhere\nand\tthere",
    "Numbers: 12345 and 007",
    "caf\xC3\xA9 r\xC3\xA9sum\xC3\xA9 na\xC3\xAFve",        /* café résumé naïve */
    "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x83\x86\xE3\x82\xAD\xE3\x82\xB9\xE3\x83\x88", /* 日本語のテキスト */
    "Mixed UTF-8: \xCE\xB1\xCE\xB2\xCE\xB3 \xCE\xB4\xCE\xB5\xCE\xB6",  /* αβγ δεζ */
    "  multiple   spaces  ",
    "don't can't won't",
    "ALLCAPS lowercase MixEd",
    "punctuation!?.,;:",
    "",
    " ",
    "a",
    "The",
    "1 2 3 4",
    "\xF0\x9F\x8E\x89 emoji test \xF0\x9F\x9A\x80",          /* 🎉 emoji test 🚀 */
};
#define NSTR (int)(sizeof(STRINGS)/sizeof(STRINGS[0]))

static void esc_print(const char *s, int n) {
    for (int i = 0; i < n; i++) {
        if (s[i] == '\n') printf("\\n");
        else if (s[i] == '\t') printf("\\t");
        else putchar((unsigned char)s[i]);
    }
}

static int real_model(const char *path) {
    gguf_file gf;
    int rc = gguf_open(&gf, path);
    if (rc != GGUF_OK) { printf("gguf_open(%s): %s\n", path, gguf_strerror(rc)); return 2; }
    tokenizer t;
    rc = tok_load(&t, &gf);
    if (rc != TOK_OK) { printf("tok_load: %s\n", tok_strerror(rc)); gguf_close(&gf); return 2; }

    printf("=== REAL-MODEL ENCODE/DECODE (SmolLM2) ===\n");
    printf("  vocab=%d merges=%d bos=%d eos=%d unk=%d pad=%d\n",
           t.n_vocab, t.n_merges, t.bos_id, t.eos_id, t.unk_id, t.pad_id);

    int all_rt = 1;
    int32_t ids[4096];
    char dec[8192];
    for (int s = 0; s < NSTR; s++) {
        size_t n = strlen(STRINGS[s]);
        int ne = tok_encode(&t, STRINGS[s], n, /*add_bos=*/0, ids, 4096);
        printf("STR[%d] :: ", s); esc_print(STRINGS[s], (int)n); printf("\n");
        if (ne < 0) { printf("ENC[%d]: ERROR %s\n", s, tok_strerror(ne)); all_rt = 0; continue; }
        printf("ENC[%d]:", s);
        for (int i = 0; i < ne; i++) printf(" %d", ids[i]);
        printf("\n");
        int dn = tok_decode(&t, ids, ne, dec, sizeof dec);
        int rt = (dn >= 0) && (dn == (int)n) && (memcmp(dec, STRINGS[s], (size_t)dn) == 0);
        printf("RT[%d]: %s", s, rt ? "OK" : "FAIL");
        if (!rt) { printf("  decoded(%d): ", dn); if (dn>0) esc_print(dec, dn); all_rt = 0; }
        printf("\n");
    }
    printf("[tok-roundtrip] %s\n", all_rt ? "PASS" : "FAIL");

    tok_free(&t);
    gguf_close(&gf);
    return all_rt ? 0 : 1;
}

int main(int argc, char **argv) {
    /* mode "enc <gguf> <text...>": print just the encoded ids of one string
     * (used by the oracle-diff path if needed). */
    if (argc >= 4 && strcmp(argv[1], "enc") == 0) {
        gguf_file gf; if (gguf_open(&gf, argv[2]) != GGUF_OK) return 2;
        tokenizer t; if (tok_load(&t, &gf) != TOK_OK) return 2;
        /* join argv[3..] with spaces */
        char text[4096]; text[0]=0;
        for (int i = 3; i < argc; i++) { if (i>3) strncat(text," ",sizeof text-strlen(text)-1); strncat(text, argv[i], sizeof text-strlen(text)-1); }
        int32_t ids[4096];
        int n = tok_encode(&t, text, strlen(text), 0, ids, 4096);
        printf("ENC:"); for (int i=0;i<n;i++) printf(" %d", ids[i]); printf("\n");
        tok_free(&t); gguf_close(&gf); return 0;
    }

    /* (1) sanity always */
    int sr = sanity_unit();
    printf("\n--- [tok-sanity] SUMMARY: %d pass / %d fail ---\n", g_pass, g_fail);
    if (sr != 0 || g_fail > 0) return 1;

    /* (2) real-model if a GGUF path is given */
    if (argc >= 2) return real_model(argv[1]);

    printf("(no GGUF arg — sanity only)\n");
    return 0;
}
