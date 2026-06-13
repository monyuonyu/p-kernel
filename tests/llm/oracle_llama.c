/*
 *  oracle_llama.c — the EXTERNAL oracle for the M1c [llm-sentence] cert.
 *
 *  Links against llama.cpp's libllama (the reference implementation). It does
 *  the SAME job our engine does, using llama.cpp's own forward + KV cache:
 *    - tokenize a FIXED prompt (add_special=true so BOS matches a base run)
 *    - greedy (argmax, temp=0) decode N tokens
 *    - print, in a machine-parseable form, the prompt token ids and the
 *      generated token ids.
 *
 *  run_forward.sh feeds the SAME prompt ids it reads here into our engine
 *  (forward_test.c) and asserts the generated id sequences are identical.
 *  Tokenizer is out of scope for M1c (M1d): we reuse llama.cpp's token ids as
 *  the fixed input, which is explicitly allowed by the brief.
 *
 *  Build: this is HOST-ONLY oracle scaffolding; it is NOT part of the p-kernel
 *  device build. It is C++-free C that includes only <llama.h>; run_forward.sh
 *  compiles & links it against the prebuilt libllama*.a.
 *
 *  Usage:  oracle_llama <model.gguf> <n_gen> <prompt text...>
 *  Output (stdout, exact lines):
 *      PROMPT: id id id ...
 *      GEN: id id id ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "llama.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <model.gguf> <n_gen> <prompt...>\n", argv[0]);
        return 2;
    }
    const char *model_path = argv[1];
    int n_gen = atoi(argv[2]);

    /* join argv[3..] into one prompt string (space-separated). */
    char prompt[4096]; prompt[0] = '\0';
    for (int i = 3; i < argc; i++) {
        if (i > 3) strncat(prompt, " ", sizeof(prompt) - strlen(prompt) - 1);
        strncat(prompt, argv[i], sizeof(prompt) - strlen(prompt) - 1);
    }

    llama_backend_init();

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "load failed\n"); return 2; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);

    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx   = 512;
    cp.n_batch = 512;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { fprintf(stderr, "ctx failed\n"); return 2; }

    int n_vocab = llama_vocab_n_tokens(vocab);

    /* tokenize: add_special=true (BOS), parse_special=false (raw text). */
    llama_token toks[1024];
    int n_tok = llama_tokenize(vocab, prompt, (int)strlen(prompt),
                               toks, 1024, /*add_special=*/true, /*parse_special=*/false);
    if (n_tok < 0) { fprintf(stderr, "tokenize failed\n"); return 2; }

    printf("PROMPT:");
    for (int i = 0; i < n_tok; i++) printf(" %d", (int)toks[i]);
    printf("\n");

    /* prefill the prompt as one batch */
    struct llama_batch batch = llama_batch_get_one(toks, n_tok);
    if (llama_decode(ctx, batch) != 0) { fprintf(stderr, "decode prompt failed\n"); return 2; }

    printf("GEN:");
    llama_token cur;
    for (int g = 0; g < n_gen; g++) {
        /* logits for the last position */
        float *logits = llama_get_logits_ith(ctx, -1);
        int best = 0; float bv = logits[0];
        for (int v = 1; v < n_vocab; v++) if (logits[v] > bv) { bv = logits[v]; best = v; }
        cur = (llama_token)best;
        printf(" %d", best);
        fflush(stdout);
        struct llama_batch nb = llama_batch_get_one(&cur, 1);
        if (llama_decode(ctx, nb) != 0) { fprintf(stderr, "decode step failed\n"); return 2; }
    }
    printf("\n");

    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
