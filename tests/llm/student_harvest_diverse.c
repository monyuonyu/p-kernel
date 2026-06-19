/*
 *  student_harvest_diverse.c — DIVERSE teacher harvest for the Cradle baby.
 *
 *  WHY: the original student_harvest.c seeds SmolLM2 with a single BOS id and
 *  decodes GREEDILY (lm_generate). At 135M the greedy continuation of a bare
 *  BOS degenerates into a near-constant loop ("the the the the ..."), so the
 *  baby distilled from it only ever learns that one repetition. This tool
 *  instead drives the SS-1 SAMPLER (lm_generate_sampled: temperature + top-k +
 *  a small repetition penalty) over a SET of varied short English prompts, each
 *  with its own RNG seed, and concatenates the decoded bytes. The result is a
 *  varied-English corpus the baby can babble less repetitively from.
 *
 *  Prompt strings -> token ids: the BPE tokenizer is NOT in the engine
 *  (forward.h, M1c scope). But SmolLM2 uses GPT-2 byte-level BPE, so we can do a
 *  small, dependency-free GREEDY-LONGEST-MATCH tokenizer here against the GGUF's
 *  own tokenizer.ggml.tokens string table: map each prompt byte to its BPE
 *  "unicode" form (bytes_to_unicode), then repeatedly take the longest vocab
 *  string that prefixes the remaining text. This is not the exact merge order,
 *  but it produces VALID token ids that prime the teacher with the intended
 *  English words — which is all we need to steer generation. The GENERATED ids
 *  are then rendered back to raw bytes by the inverse map (same as the original
 *  harvest), so the corpus is real teacher text.
 *
 *  Build (wave-49 discipline): -O1 -ffp-contract=off.
 *  Usage: student_harvest_diverse <model.gguf> <out_path> [per_prompt_tokens]
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../arch/common/llm/gguf.h"
#include "../../arch/common/llm/forward.h"

/* ---- GPT-2 byte<->unicode bijection (forward and inverse) ---------------- */
static int g_cp2byte[512];          /* unicode codepoint -> original byte     */
static int g_byte2cp[256];          /* original byte    -> unicode codepoint  */
static void build_byte_maps(void)
{
    for (int i = 0; i < 512; i++) g_cp2byte[i] = -1;
    int n = 0;
    for (int b = 0; b < 256; b++) {
        int keep = (b >= '!' && b <= '~') || (b >= 0xA1 && b <= 0xAC) || (b >= 0xAE && b <= 0xFF);
        if (keep) { g_cp2byte[b] = b; g_byte2cp[b] = b; }
    }
    for (int b = 0; b < 256; b++) {
        int keep = (b >= '!' && b <= '~') || (b >= 0xA1 && b <= 0xAC) || (b >= 0xAE && b <= 0xFF);
        if (!keep) { g_cp2byte[256 + n] = b; g_byte2cp[b] = 256 + n; n++; }
    }
}
/* decode one UTF-8 codepoint at s[*i]; advance *i; return codepoint or -1. */
static int utf8_next(const uint8_t *s, uint64_t L, uint64_t *i)
{
    if (*i >= L) return -1;
    uint8_t c = s[*i];
    if (c < 0x80) { (*i)++; return c; }
    if ((c & 0xE0) == 0xC0 && *i + 1 < L) {
        int cp = ((c & 0x1F) << 6) | (s[*i+1] & 0x3F); *i += 2; return cp;
    }
    if ((c & 0xF0) == 0xE0 && *i + 2 < L) {
        int cp = ((c & 0x0F) << 12) | ((s[*i+1] & 0x3F) << 6) | (s[*i+2] & 0x3F);
        *i += 3; return cp;
    }
    (*i)++; return c;
}
/* encode one codepoint as UTF-8 into out (max 3 bytes used); return n bytes. */
static int utf8_put(int cp, uint8_t *out)
{
    if (cp < 0x80) { out[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800) { out[0] = 0xC0 | (cp >> 6); out[1] = 0x80 | (cp & 0x3F); return 2; }
    out[0] = 0xE0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 0x3F);
    out[2] = 0x80 | (cp & 0x3F); return 3;
}

/* token-string table view into the mmap */
static const uint8_t **g_tok_ptr = NULL;
static uint64_t       *g_tok_len = NULL;
static uint64_t        g_n_toks  = 0;

/* GREEDY-LONGEST-MATCH tokenize an ASCII prompt into ids. The teacher uses a
 * leading-space convention for word starts (GPT-2). We do NOT prepend a space
 * here; callers write prompts with their own leading-space semantics. Returns
 * the number of ids written (<= cap). Unmatched bytes are skipped. */
static int tokenize_greedy(const char *text, int *ids, int cap)
{
    /* render the prompt bytes into BPE-unicode UTF-8 first */
    uint8_t buf[1024]; uint64_t blen = 0;
    for (const char *p = text; *p && blen + 3 < sizeof(buf); p++) {
        int cp = g_byte2cp[(unsigned char)*p];
        blen += (uint64_t)utf8_put(cp, buf + blen);
    }
    int n = 0; uint64_t pos = 0;
    while (pos < blen && n < cap) {
        uint64_t best_id = (uint64_t)-1, best_len = 0;
        for (uint64_t t = 0; t < g_n_toks; t++) {
            uint64_t L = g_tok_len[t];
            if (L == 0 || L > blen - pos || L <= best_len) continue;
            if (memcmp(g_tok_ptr[t], buf + pos, L) == 0) { best_len = L; best_id = t; }
        }
        if (best_id == (uint64_t)-1) { pos++; continue; }  /* skip 1 byte */
        ids[n++] = (int)best_id; pos += best_len;
    }
    return n;
}

/* render generated ids -> raw bytes into FILE, return bytes written */
static long render_ids(const int *gen, int g, FILE *f)
{
    long out = 0;
    for (int i = 0; i < g; i++) {
        int id = gen[i];
        if (id < 0 || (uint64_t)id >= g_n_toks) continue;
        const uint8_t *s = g_tok_ptr[id]; uint64_t L = g_tok_len[id];
        uint64_t k = 0;
        while (k < L) {
            int cp = utf8_next(s, L, &k);
            if (cp < 0) break;
            int b = (cp >= 0 && cp < 512) ? g_cp2byte[cp] : -1;
            if (b < 0) b = cp & 0xff;
            fputc(b, f); out++;
        }
    }
    return out;
}

/* The varied prompt set. Leading spaces follow the GPT-2 word-start convention
 * so the words tokenize to their natural (space-prefixed) ids. Two dozen short,
 * topically diverse openers => varied continuations. */
static const char *PROMPTS[] = {
    "The cat",            " Once upon a time",   " Water is",
    " My favorite",       " The sky was",        " People learn",
    " In the morning",    " She opened the",     " Science tells us",
    " The old house",     " A long time ago",    " Music can",
    " The river flows",   " Children love to",   " When winter comes",
    " The best way to",   " History shows that", " Birds fly",
    " The forest was",    " He looked at the",   " Cooking is",
    " The city lights",   " Every day the",      " Far away,",
};

int main(int argc, char **argv)
{
    build_byte_maps();
    if (argc < 3) {
        fprintf(stderr, "usage: %s <model.gguf> <out_path> [per_prompt_tokens]\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    const char *out  = argv[2];
    int per = (argc > 3) ? atoi(argv[3]) : 60;
    if (per < 8)  per = 8;
    if (per > 96) per = 96;          /* keep bounded; this is SLOW */

    gguf_file gf;
    if (gguf_open(&gf, path) != 0) { fprintf(stderr, "[harvest] gguf_open failed\n"); return 1; }
    lm_model m;
    int rc = lm_load(&m, &gf);
    if (rc != LM_OK) { fprintf(stderr, "[harvest] lm_load: %s\n", lm_strerror(rc)); gguf_close(&gf); return 1; }

    /* resolve token-string table */
    {
        const gguf_kv *kv = gguf_find(&gf, "tokenizer.ggml.tokens");
        if (kv && kv->type == GGUF_T_ARRAY && kv->arr_type == GGUF_T_STRING) {
            g_n_toks = kv->arr_len;
            g_tok_ptr = (const uint8_t **)malloc(g_n_toks * sizeof(*g_tok_ptr));
            g_tok_len = (uint64_t *)malloc(g_n_toks * sizeof(*g_tok_len));
            const uint8_t *p = kv->arr_data;
            const uint8_t *end = gf.base + gf.size;
            for (uint64_t i = 0; i < g_n_toks && p + 8 <= end; i++) {
                uint64_t len; memcpy(&len, p, 8); p += 8;
                if (p + len > end) { g_n_toks = i; break; }
                g_tok_ptr[i] = p; g_tok_len[i] = len; p += len;
            }
        }
    }
    if (!g_tok_ptr || g_n_toks == 0) {
        fprintf(stderr, "[harvest] token strings unavailable — cannot do diverse harvest\n");
        lm_free(&m); gguf_close(&gf); return 1;
    }

    FILE *f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "[harvest] cannot open %s\n", out); lm_free(&m); gguf_close(&gf); return 1; }

    const int NP = (int)(sizeof(PROMPTS) / sizeof(PROMPTS[0]));
    int *gen = (int *)malloc(sizeof(int) * per);
    int *seed_ids = (int *)malloc(sizeof(int) * 64);
    long total = 0;
    /* SS-1 sampler knobs: warm-ish temp, modest top-k, light rep-penalty so the
     * teacher stays coherent English but does not lock into a single phrase. */
    const float TEMP = 0.8f;
    const int   TOPK = 40;
    const float TOPP = 0.95f;
    const float REPP = 1.3f;
    const int   EOS  = 2;             /* SmolLM2 eos */

    for (int pi = 0; pi < NP; pi++) {
        int n_seed = tokenize_greedy(PROMPTS[pi], seed_ids, 64);
        if (n_seed == 0) { seed_ids[0] = 1; n_seed = 1; }  /* BOS fallback */
        /* a distinct, deterministic seed per prompt for reproducibility */
        uint64_t seed = 0x9E3779B97F4A7C15ULL * (uint64_t)(pi + 1);
        int g = lm_generate_sampled(&m, seed_ids, n_seed, gen, per,
                                    TEMP, TOPK, TOPP, REPP, EOS, seed);
        if (g < 0) { fprintf(stderr, "[harvest] prompt %d sampled gen failed (%d)\n", pi, g); continue; }
        /* write the prompt text itself first (real English context), then the
         * teacher's sampled continuation. */
        for (const char *p = PROMPTS[pi]; *p; p++) { fputc((unsigned char)*p, f); total++; }
        total += render_ids(gen, g, f);
        fputc(' ', f); total++;       /* separator between samples */
        fprintf(stderr, "[harvest] prompt %2d/%d (%-22.22s) seeded %2d ids -> %d tok\n",
                pi + 1, NP, PROMPTS[pi], n_seed, g);
    }
    fclose(f);
    fprintf(stderr, "[harvest] DIVERSE corpus: %d prompts -> %ld bytes -> %s\n", NP, total, out);

    free(gen); free(seed_ids);
    free(g_tok_ptr); free(g_tok_len);
    lm_free(&m); gguf_close(&gf);
    return 0;
}
