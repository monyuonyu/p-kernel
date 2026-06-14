/*
 *  student_harvest.c — ONE-TIME teacher harvest for the NS-1 Cradle baby.
 *
 *  The teacher->byte bridge (native-student.md §B.6 (a), SEQUENCE-LEVEL): run
 *  SmolLM2-135M (forward.c, M1c) to greedily generate a stretch of text, and
 *  write its RAW BYTES to a fixture file. The baby (student.c) then learns to
 *  predict the next byte of the teacher's text — "learn to babble like the
 *  teacher talks". This side-steps the teacher(50k BPE) vs baby(256 byte)
 *  vocab mismatch entirely (the §A.5 honest gap, deferred to a later wave).
 *
 *  This is SLOW (~250ms/token) and run ONCE by run_student.sh; the cert loads
 *  the fixture and never re-runs the teacher.
 *
 *  NOTE on tokenization (M1c scope, forward.h): the BPE tokenizer is NOT in the
 *  engine. So we cannot turn arbitrary English into teacher token ids here.
 *  Instead we SEED the teacher with a fixed short id-prefix (BOS-ish + a couple
 *  of common ids) and let it greedily generate; the *generated* token ids are
 *  then rendered to bytes via the GGUF's token-string table (gguf metadata
 *  tokenizer.ggml.tokens), giving real teacher-produced text BYTES. If the
 *  token strings are unavailable we fall back to writing the raw little-endian
 *  token-id bytes (still teacher-derived structure, honestly cruder).
 *
 *  Build (wave-49): -O1 -ffp-contract=off.
 *  Usage: student_harvest <model.gguf> <n_tokens> <out_path>
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../arch/common/llm/gguf.h"
#include "../../arch/common/llm/forward.h"

/* GPT-2 / SmolLM2 byte-level BPE uses a bijection bytes<->unicode so every
 * byte renders as a printable codepoint (e.g. space 0x20 -> U+0120 'Ġ', newline
 * 0x0A -> U+010A). build the INVERSE map codepoint -> original byte. */
static int g_cp2byte[512];
static void build_byte_decoder(void)
{
    for (int i = 0; i < 512; i++) g_cp2byte[i] = -1;
    int n = 0;
    /* the "kept as themselves" ranges, exactly as GPT-2's bytes_to_unicode() */
    for (int b = 0; b < 256; b++) {
        int keep = (b >= '!' && b <= '~') || (b >= 0xA1 && b <= 0xAC) || (b >= 0xAE && b <= 0xFF);
        if (keep) g_cp2byte[b] = b;
    }
    for (int b = 0; b < 256; b++) {
        int keep = (b >= '!' && b <= '~') || (b >= 0xA1 && b <= 0xAC) || (b >= 0xAE && b <= 0xFF);
        if (!keep) { g_cp2byte[256 + n] = b; n++; }
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
    (*i)++; return c;   /* malformed: pass the raw byte */
}

int main(int argc, char **argv)
{
    build_byte_decoder();
    if (argc < 4) {
        fprintf(stderr, "usage: %s <model.gguf> <n_tokens> <out_path>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    int n_tokens = atoi(argv[2]);
    const char *out = argv[3];
    if (n_tokens < 1) n_tokens = 256;

    gguf_file gf;
    if (gguf_open(&gf, path) != 0) {
        fprintf(stderr, "[harvest] gguf_open failed\n");
        return 1;
    }
    lm_model m;
    int rc = lm_load(&m, &gf);
    if (rc != LM_OK) {
        fprintf(stderr, "[harvest] lm_load: %s\n", lm_strerror(rc));
        gguf_close(&gf);
        return 1;
    }

    /* Resolve the tokenizer token-string table for id->text rendering. M1a
     * (gguf.c) keeps arrays unexpanded (raw arr_data), so we walk the STRING
     * array ourselves: build, for each token id, a (ptr,len) view into the
     * mmap. Cheap one-pass scan. */
    const uint8_t **tok_ptr = NULL; uint64_t *tok_len = NULL; uint64_t n_toks = 0;
    {
        const gguf_kv *kv = gguf_find(&gf, "tokenizer.ggml.tokens");
        if (kv && kv->type == GGUF_T_ARRAY && kv->arr_type == GGUF_T_STRING) {
            n_toks = kv->arr_len;
            tok_ptr = (const uint8_t **)malloc(n_toks * sizeof(*tok_ptr));
            tok_len = (uint64_t *)malloc(n_toks * sizeof(*tok_len));
            const uint8_t *p = kv->arr_data;
            const uint8_t *end = gf.base + gf.size;
            for (uint64_t i = 0; i < n_toks && p + 8 <= end; i++) {
                uint64_t len; memcpy(&len, p, 8); p += 8;
                if (p + len > end) { n_toks = i; break; }
                tok_ptr[i] = p; tok_len[i] = len; p += len;
            }
        }
    }

    /* seed prompt: a few low/common ids. SmolLM2 BOS is typically id 1; we keep
     * it minimal and let greedy decoding do the talking. */
    int seed_ids[] = { 1 };
    int n_seed = (int)(sizeof(seed_ids) / sizeof(seed_ids[0]));

    int *gen = (int *)malloc(sizeof(int) * n_tokens);
    if (!gen) { lm_free(&m); gguf_close(&gf); return 1; }
    int g = lm_generate(&m, seed_ids, n_seed, gen, n_tokens);
    if (g < 0) {
        fprintf(stderr, "[harvest] lm_generate failed (%d)\n", g);
        free(gen); lm_free(&m); gguf_close(&gf); return 1;
    }

    FILE *f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "[harvest] cannot open %s\n", out); free(gen); lm_free(&m); gguf_close(&gf); return 1; }

    long bytes_out = 0;
    if (tok_ptr && n_toks > 0) {
        /* render each generated id to its token string, decoding GPT-2 byte-
         * level BPE back to the ORIGINAL raw bytes (so the fixture is the real
         * text the teacher produced, the cleanest sequence-level signal). */
        for (int i = 0; i < g; i++) {
            int id = gen[i];
            if (id < 0 || (uint64_t)id >= n_toks) continue;
            const uint8_t *s = tok_ptr[id]; uint64_t L = tok_len[id];
            uint64_t k = 0;
            while (k < L) {
                int cp = utf8_next(s, L, &k);
                if (cp < 0) break;
                int b = (cp >= 0 && cp < 512) ? g_cp2byte[cp] : -1;
                if (b < 0) b = cp & 0xff;     /* fallback for special tokens */
                fputc(b, f); bytes_out++;
            }
        }
    }
    if (bytes_out == 0) {
        /* fallback: raw little-endian token ids as bytes (cruder but teacher-derived) */
        for (int i = 0; i < g; i++) {
            unsigned id = (unsigned)gen[i];
            fputc(id & 0xff, f); fputc((id >> 8) & 0xff, f); bytes_out += 2;
        }
        fprintf(stderr, "[harvest] token strings unavailable; wrote raw id bytes\n");
    }
    fclose(f);
    fprintf(stderr, "[harvest] %d tokens -> %ld bytes\n", g, bytes_out);

    free(gen);
    free(tok_ptr); free(tok_len);
    lm_free(&m);
    gguf_close(&gf);
    return 0;
}
