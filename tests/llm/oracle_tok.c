/*
 *  oracle_tok.c — the EXTERNAL oracle for the M1d [tok-encode] cert.
 *
 *  Links against llama.cpp's libllama (the reference implementation) and runs
 *  llama_tokenize() / llama_detokenize() on the SAME FIXED strings that
 *  tokenizer_test.c encodes with our tokenizer. run_tokenizer.sh diffs the
 *  "ENC[i]:" lines token-for-token. add_special=false, parse_special=false
 *  (raw text, the M1c-oracle convention; SmolLM2 has add_bos_token=false).
 *
 *  The STRINGS[] list MUST stay byte-identical to tokenizer_test.c's STRINGS[].
 *
 *  HOST-ONLY oracle scaffolding (same tier as oracle_llama.c): NOT part of the
 *  p-kernel device build. C-only, includes only <llama.h>; run_tokenizer.sh
 *  compiles & links it against the prebuilt libllama*.a.
 *
 *  Usage:  oracle_tok <model.gguf>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "llama.h"

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
#define NS (int)(sizeof(STRINGS)/sizeof(STRINGS[0]))

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s model.gguf\n", argv[0]); return 2; }
    llama_backend_init();
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(argv[1], mp);
    if (!model) { fprintf(stderr, "load failed\n"); return 2; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);

    for (int s = 0; s < NS; s++) {
        const char *t = STRINGS[s];
        llama_token toks[2048];
        int n = llama_tokenize(vocab, t, (int)strlen(t), toks, 2048,
                               /*add_special=*/false, /*parse_special=*/false);
        printf("STR[%d] len=%zu :: ", s, strlen(t));
        /* escape the string for one-line printing */
        for (const char *p = t; *p; p++) {
            if (*p == '\n') printf("\\n");
            else if (*p == '\t') printf("\\t");
            else putchar((unsigned char)*p);
        }
        printf("\nENC[%d]:", s);
        for (int i = 0; i < n; i++) printf(" %d", (int)toks[i]);
        printf("\n");
        /* decode back */
        char buf[4096];
        int dn = llama_detokenize(vocab, toks, n, buf, sizeof(buf),
                                  /*remove_special=*/false, /*unparse_special=*/false);
        printf("DEC[%d] len=%d :: ", s, dn);
        for (int i = 0; i < dn; i++) {
            if (buf[i] == '\n') printf("\\n");
            else if (buf[i] == '\t') printf("\\t");
            else putchar((unsigned char)buf[i]);
        }
        printf("\n");
    }
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
