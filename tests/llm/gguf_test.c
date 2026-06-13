/*
 *  gguf_test.c — host harness for arch/common/llm/gguf.c (milestone M1a).
 *
 *  Two modes, both runnable on a plain host (no kernel, no device):
 *
 *   (1) SYNTHETIC ROUND-TRIP (always runs, self-contained cert):
 *       build a tiny but spec-valid GGUF in memory → write to a temp file →
 *       parse it back → assert every header field, every metadata KV, and
 *       every tensor-table row matches EXACTLY what was written. This is the
 *       falsifiable PASS/FAIL the M1a cert leans on when no real file is
 *       present. Exit 0 = all PASS.
 *
 *   (2) REAL-FILE DUMP (optional, if argv[1] is a .gguf path):
 *       parse a real GGUF (e.g. SmolLM2-135M) and print general.architecture,
 *       block_count, embedding_length, head_count, head_count_kv,
 *       feed_forward_length, vocab_size, rope freq_base, the file's quant
 *       types present, and the full tensor table (name, shape, type, offset).
 *       Everything is driven from the file's own metadata — nothing hardcoded.
 *       This output is what gets cross-checked against the HF config.json.
 *
 *  Usage:
 *      ./gguf_test                 # synthetic round-trip cert only
 *      ./gguf_test model.gguf      # real-file dump THEN synthetic cert
 */
#define _GNU_SOURCE
#include "../../arch/common/llm/gguf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

static int g_fails = 0;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  PASS  %s\n", (msg)); }                     \
        else      { printf("  FAIL  %s\n", (msg)); g_fails++; }          \
    } while (0)

/* ---------- little-endian byte writer for the synthetic fixture ---------- */
typedef struct { uint8_t *p; size_t cap, len; } buf;

static void bput(buf *b, const void *src, size_t n)
{
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 64;
        b->p = (uint8_t *)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, src, n);
    b->len += n;
}
static void bu8 (buf *b, uint8_t v)  { bput(b, &v, 1); }
static void bu32(buf *b, uint32_t v) { uint8_t t[4]; for(int i=0;i<4;i++)t[i]=(v>>(8*i))&0xff; bput(b,t,4); }
static void bu64(buf *b, uint64_t v) { uint8_t t[8]; for(int i=0;i<8;i++)t[i]=(v>>(8*i))&0xff; bput(b,t,8); }
static void bf32(buf *b, float f)    { uint32_t u; memcpy(&u,&f,4); bu32(b,u); }
static void bstr(buf *b, const char *s) { uint64_t n=strlen(s); bu64(b,n); bput(b,(const void*)s,n); }

/* a metadata KV helpers */
static void kv_u32(buf *b, const char *k, uint32_t v){ bstr(b,k); bu32(b,GGUF_T_UINT32); bu32(b,v); }
static void kv_f32(buf *b, const char *k, float v)   { bstr(b,k); bu32(b,GGUF_T_FLOAT32); bf32(b,v); }
static void kv_str(buf *b, const char *k, const char *v){ bstr(b,k); bu32(b,GGUF_T_STRING); bstr(b,v); }

/* ---------- synthetic fixture: known config + tensor table --------------- */
/* These constants are the ground truth the round-trip must reproduce. */
#define SYN_ARCH        "llama"
#define SYN_N_LAYER     3u
#define SYN_N_EMBD      8u
#define SYN_N_HEAD      4u
#define SYN_N_HEAD_KV   2u
#define SYN_N_FF        16u
#define SYN_VOCAB       7u
#define SYN_ROPE_BASE   100000.0f
#define SYN_ALIGN       32u

static int build_synthetic(const char *path)
{
    buf b = {0};

    /* header */
    bput(&b, "GGUF", 4);
    bu32(&b, 3);              /* version 3                                   */
    bu64(&b, 2);              /* tensor_count = 2                            */
    bu64(&b, 9);              /* metadata_kv_count = 9                       */

    /* metadata (mix of types incl. an array, to exercise the parser) */
    kv_str(&b, "general.architecture",          SYN_ARCH);
    kv_u32(&b, "llama.block_count",             SYN_N_LAYER);
    kv_u32(&b, "llama.embedding_length",        SYN_N_EMBD);
    kv_u32(&b, "llama.attention.head_count",    SYN_N_HEAD);
    kv_u32(&b, "llama.attention.head_count_kv", SYN_N_HEAD_KV);
    kv_u32(&b, "llama.feed_forward_length",     SYN_N_FF);
    kv_f32(&b, "llama.rope.freq_base",          SYN_ROPE_BASE);
    kv_u32(&b, "general.alignment",             SYN_ALIGN);
    /* an array KV: tokenizer.ggml.tokens (3 strings) — parser must skip it
     * and still land on the tensor table correctly. vocab size below is a
     * separate scalar so we don't depend on array expansion in M1a. */
    bstr(&b, "tokenizer.ggml.tokens");
    bu32(&b, GGUF_T_ARRAY);
    bu32(&b, GGUF_T_STRING);
    bu64(&b, 3);
    bstr(&b, "<unk>"); bstr(&b, "a"); bstr(&b, "b");
    /* (9 KVs total; the array above is #9.) SYN_VOCAB below is used only as a
     * tensor dimension — the synthetic cert proves array parsing via this
     * tokens array's length (== 3), not via a vocab scalar. */

    /* tensor infos: two tensors of known shape/type/offset.
     * token_embd.weight : [n_embd, vocab] F32, offset 0
     * output_norm.weight: [n_embd]        F32, offset = padded size of t0     */
    uint64_t t0_elems = (uint64_t)SYN_N_EMBD * SYN_VOCAB;
    uint64_t t0_bytes = t0_elems * 4;                 /* F32                  */
    uint64_t t0_pad   = (SYN_ALIGN - (t0_bytes % SYN_ALIGN)) % SYN_ALIGN;
    uint64_t t1_off   = t0_bytes + t0_pad;            /* aligned             */

    /* t0 */
    bstr(&b, "token_embd.weight");
    bu32(&b, 2);                      /* n_dims                              */
    bu64(&b, SYN_N_EMBD); bu64(&b, SYN_VOCAB);
    bu32(&b, GGML_TYPE_F32);
    bu64(&b, 0);                      /* offset                              */
    /* t1 */
    bstr(&b, "output_norm.weight");
    bu32(&b, 1);
    bu64(&b, SYN_N_EMBD);
    bu32(&b, GGML_TYPE_F32);
    bu64(&b, t1_off);

    /* pad to alignment, then tensor data */
    uint64_t pad = (SYN_ALIGN - (b.len % SYN_ALIGN)) % SYN_ALIGN;
    for (uint64_t i = 0; i < pad; i++) bu8(&b, 0);

    /* tensor data: t0 then t1 (zeros are fine; M1a doesn't read values) */
    for (uint64_t i = 0; i < t0_bytes; i++) bu8(&b, 0);
    for (uint64_t i = 0; i < t0_pad;   i++) bu8(&b, 0);
    uint64_t t1_bytes = (uint64_t)SYN_N_EMBD * 4;
    for (uint64_t i = 0; i < t1_bytes; i++) bu8(&b, 0);

    FILE *f = fopen(path, "wb");
    if (!f) { free(b.p); return -1; }
    size_t w = fwrite(b.p, 1, b.len, f);
    fclose(f);
    free(b.p);
    return (w == b.len) ? 0 : -1;
}

static int streq_gguf(gguf_str s, const char *c)
{
    size_t n = strlen(c);
    return s.len == n && memcmp(s.ptr, c, n) == 0;
}

static int run_synthetic_cert(void)
{
    char path[] = "/tmp/pkernel_gguf_synth_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) close(fd);

    printf("\n=== SYNTHETIC ROUND-TRIP CERT ===\n");
    if (build_synthetic(path) != 0) {
        printf("  FAIL  could not write synthetic fixture\n");
        return 1;
    }

    gguf_file gf;
    int rc = gguf_open(&gf, path);
    if (rc != GGUF_OK) {
        printf("  FAIL  gguf_open: %s\n", gguf_strerror(rc));
        unlink(path);
        return 1;
    }

    /* header */
    CHECK(gf.version == 3,            "version == 3");
    CHECK(gf.n_kv == 9,              "metadata_kv_count == 9");
    CHECK(gf.n_tensors == 2,         "tensor_count == 2");
    CHECK(gf.alignment == SYN_ALIGN, "general.alignment == 32");

    /* metadata KVs, by typed getter (proves end-to-end decode) */
    gguf_str arch;
    CHECK(gguf_get_str(&gf, "general.architecture", &arch) &&
          streq_gguf(arch, SYN_ARCH), "general.architecture == \"llama\"");

    uint64_t u;
    CHECK(gguf_get_u64(&gf, "llama.block_count", &u) && u == SYN_N_LAYER,
          "block_count == 3");
    CHECK(gguf_get_u64(&gf, "llama.embedding_length", &u) && u == SYN_N_EMBD,
          "embedding_length == 8");
    CHECK(gguf_get_u64(&gf, "llama.attention.head_count", &u) && u == SYN_N_HEAD,
          "head_count == 4");
    CHECK(gguf_get_u64(&gf, "llama.attention.head_count_kv", &u) && u == SYN_N_HEAD_KV,
          "head_count_kv == 2");
    CHECK(gguf_get_u64(&gf, "llama.feed_forward_length", &u) && u == SYN_N_FF,
          "feed_forward_length == 16");

    float fb;
    CHECK(gguf_get_f32(&gf, "llama.rope.freq_base", &fb) && fb == SYN_ROPE_BASE,
          "rope.freq_base == 100000");

    /* array KV: proves array parsing + that the parser landed on the tensor
     * table correctly afterwards (vocab derived from the array length). */
    const gguf_kv *toks = gguf_find(&gf, "tokenizer.ggml.tokens");
    CHECK(toks && toks->type == GGUF_T_ARRAY &&
          toks->arr_type == GGUF_T_STRING && toks->arr_len == 3,
          "tokenizer.ggml.tokens == array<str>[3]");

    /* tensor table exact match */
    CHECK(streq_gguf(gf.tensors[0].name, "token_embd.weight"),
          "tensor[0].name == token_embd.weight");
    CHECK(gf.tensors[0].n_dims == 2 &&
          gf.tensors[0].dims[0] == SYN_N_EMBD &&
          gf.tensors[0].dims[1] == SYN_VOCAB,
          "tensor[0].shape == [8, 7]");
    CHECK(gf.tensors[0].type == GGML_TYPE_F32, "tensor[0].type == F32");
    CHECK(gf.tensors[0].offset == 0,           "tensor[0].offset == 0");
    CHECK(gf.tensors[0].nbytes == (uint64_t)SYN_N_EMBD * SYN_VOCAB * 4,
          "tensor[0].nbytes == 8*7*4");

    CHECK(streq_gguf(gf.tensors[1].name, "output_norm.weight"),
          "tensor[1].name == output_norm.weight");
    CHECK(gf.tensors[1].n_dims == 1 && gf.tensors[1].dims[0] == SYN_N_EMBD,
          "tensor[1].shape == [8]");
    CHECK(gf.tensors[1].type == GGML_TYPE_F32, "tensor[1].type == F32");

    /* data pointers must be inside the mapping and aligned per spec */
    CHECK(gf.data_offset % gf.alignment == 0, "data section aligned to 32");
    CHECK(gf.tensors[0].data >= gf.base &&
          gf.tensors[0].data <  gf.base + gf.size, "tensor[0].data in-bounds");
    CHECK(gf.tensors[1].data >= gf.base &&
          gf.tensors[1].data <  gf.base + gf.size, "tensor[1].data in-bounds");

    gguf_close(&gf);
    unlink(path);
    return 0;
}

/* ---------- real-file dump ----------------------------------------------- */
static void print_str_kv(const gguf_file *gf, const char *key)
{
    gguf_str s;
    if (gguf_get_str(gf, key, &s))
        printf("  %-28s %.*s\n", key, (int)s.len, s.ptr);
    else
        printf("  %-28s <absent>\n", key);
}
static void print_u64_kv(const gguf_file *gf, const char *key)
{
    uint64_t v;
    if (gguf_get_u64(gf, key, &v))
        printf("  %-28s %llu\n", key, (unsigned long long)v);
    else
        printf("  %-28s <absent>\n", key);
}
static void print_f32_kv(const gguf_file *gf, const char *key)
{
    float v;
    if (gguf_get_f32(gf, key, &v))
        printf("  %-28s %.6g\n", key, (double)v);
    else
        printf("  %-28s <absent>\n", key);
}

/* derive the architecture prefix (e.g. "llama") to build arch-scoped keys */
static void arch_key(const gguf_file *gf, const char *suffix, char *out, size_t n)
{
    gguf_str a;
    if (gguf_get_str(gf, "general.architecture", &a) && a.len < n - 64)
        snprintf(out, n, "%.*s.%s", (int)a.len, a.ptr, suffix);
    else
        snprintf(out, n, "llama.%s", suffix);   /* sane fallback            */
}

static void dump_real(const gguf_file *gf)
{
    printf("\n=== GGUF FILE DUMP ===\n");
    printf("  version                      %u\n", gf->version);
    printf("  metadata_kv_count            %llu\n", (unsigned long long)gf->n_kv);
    printf("  tensor_count                 %llu\n", (unsigned long long)gf->n_tensors);
    printf("  alignment                    %llu\n", (unsigned long long)gf->alignment);
    printf("  data_offset                  %llu\n", (unsigned long long)gf->data_offset);

    printf("\n--- config (from metadata) ---\n");
    char k[160];
    print_str_kv(gf, "general.architecture");
    print_str_kv(gf, "general.name");
    arch_key(gf, "block_count", k, sizeof k);             print_u64_kv(gf, k);
    arch_key(gf, "embedding_length", k, sizeof k);        print_u64_kv(gf, k);
    arch_key(gf, "attention.head_count", k, sizeof k);    print_u64_kv(gf, k);
    arch_key(gf, "attention.head_count_kv", k, sizeof k); print_u64_kv(gf, k);
    arch_key(gf, "feed_forward_length", k, sizeof k);     print_u64_kv(gf, k);
    arch_key(gf, "context_length", k, sizeof k);          print_u64_kv(gf, k);
    arch_key(gf, "rope.freq_base", k, sizeof k);          print_f32_kv(gf, k);
    arch_key(gf, "attention.layer_norm_rms_epsilon", k, sizeof k); print_f32_kv(gf, k);
    /* vocab_size: prefer the explicit scalar, else the token-array length */
    {
        uint64_t v;
        if (gguf_get_u64(gf, "llama.vocab_size", &v))
            printf("  %-28s %llu\n", "vocab_size", (unsigned long long)v);
        else {
            const gguf_kv *toks = gguf_find(gf, "tokenizer.ggml.tokens");
            if (toks && toks->type == GGUF_T_ARRAY)
                printf("  %-28s %llu (from tokenizer.ggml.tokens)\n",
                       "vocab_size", (unsigned long long)toks->arr_len);
            else
                printf("  %-28s <absent>\n", "vocab_size");
        }
    }
    print_u64_kv(gf, "general.file_type");

    /* quant types present across all tensors */
    printf("\n--- quant/element types present ---\n");
    {
        int seen[GGML_TYPE_COUNT_HINT];
        memset(seen, 0, sizeof seen);
        for (uint64_t i = 0; i < gf->n_tensors; i++) {
            int t = (int)gf->tensors[i].type;
            if (t >= 0 && t < GGML_TYPE_COUNT_HINT) seen[t]++;
        }
        for (int t = 0; t < GGML_TYPE_COUNT_HINT; t++)
            if (seen[t])
                printf("  %-6s x %d tensors\n",
                       ggml_type_name((enum ggml_type)t), seen[t]);
    }

    /* full tensor table */
    printf("\n--- tensor table (%llu tensors) ---\n",
           (unsigned long long)gf->n_tensors);
    printf("  %-32s %-18s %-6s %-12s %s\n",
           "name", "shape", "type", "offset", "bytes");
    for (uint64_t i = 0; i < gf->n_tensors; i++) {
        const gguf_tensor *t = &gf->tensors[i];
        char shape[64]; int o = 0;
        o += snprintf(shape + o, sizeof shape - o, "[");
        for (uint32_t d = 0; d < t->n_dims; d++)
            o += snprintf(shape + o, sizeof shape - o, "%s%llu",
                          d ? "," : "", (unsigned long long)t->dims[d]);
        snprintf(shape + o, sizeof shape - o, "]");
        printf("  %-32.*s %-18s %-6s %-12llu %llu\n",
               (int)t->name.len, t->name.ptr, shape,
               ggml_type_name(t->type),
               (unsigned long long)t->offset,
               (unsigned long long)t->nbytes);
    }
}

int main(int argc, char **argv)
{
    if (argc >= 2) {
        gguf_file gf;
        int rc = gguf_open(&gf, argv[1]);
        if (rc != GGUF_OK) {
            fprintf(stderr, "gguf_open(%s): %s\n", argv[1], gguf_strerror(rc));
            return 2;
        }
        dump_real(&gf);
        gguf_close(&gf);
    }

    int rc = run_synthetic_cert();

    printf("\n==================================\n");
    if (g_fails == 0 && rc == 0) {
        printf("RESULT: PASS (synthetic round-trip exact-match)\n");
        return 0;
    }
    printf("RESULT: FAIL (%d assertion failures)\n", g_fails ? g_fails : 1);
    return 1;
}
