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

int main(int argc, char **argv)
{
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

    /* try to resolve the tokenizer token-string table for id->text rendering */
    const char **toks = NULL; int n_toks = 0;
    gguf_get_str_array(&gf, "tokenizer.ggml.tokens", &toks, &n_toks);

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
    if (toks && n_toks > 0) {
        /* render each generated id to its token string; SentencePiece uses
         * U+2581 (0xE2 0x96 0x81) for a leading space — translate to ' '. */
        for (int i = 0; i < g; i++) {
            int id = gen[i];
            if (id < 0 || id >= n_toks || !toks[id]) continue;
            const char *s = toks[id];
            for (const char *p = s; *p; ) {
                if ((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x96 &&
                    (unsigned char)p[2] == 0x81) { fputc(' ', f); p += 3; bytes_out++; }
                else { fputc((unsigned char)*p, f); p++; bytes_out++; }
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
    if (toks) free((void *)toks);
    lm_free(&m);
    gguf_close(&gf);
    return 0;
}
