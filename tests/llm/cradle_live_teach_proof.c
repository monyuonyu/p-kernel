/*
 *  cradle_live_teach_proof.c — CT-2 (conversational-teaching.md §7): does a
 *                              REAL live-generated teacher lesson beat the
 *                              fixture-only baseline?
 *
 *  Standalone HOST harness. Links the SmolLM2 engine (arch/common/llm/
 *  llm_shell.c + gguf/tokenizer/forward/quant/pk_parallel/sample) to GENERATE
 *  one real completion, then hands the raw bytes to cradle_live_teach_test()
 *  (arch/common/llm/cradle.c), which stays gguf/forward-free by design (see
 *  cradle.c's file header) and does the held-out-loss math via student.c.
 *
 *  HONEST SCOPE: this is NOT the [cradle-teach] cert (cradle_teach_proof.c),
 *  which proves the teacher->student BRIDGE with a crafted fact/probe pair
 *  and a strong generalization claim. This cert asks a narrower, more honest
 *  question: does training on a real generated document lower loss on an
 *  UNSEEN TAIL OF THAT SAME DOCUMENT, more than a fixture-only baseline that
 *  never saw it at all? It says nothing about generalizing to a paraphrase.
 *
 *  Needs PKERNEL_LLM_GGUF set to a real SmolLM2 GGUF; without one, SKIPS
 *  (exit 0) rather than failing — this cert is about the TRAINING COMPARISON,
 *  not about proving the engine loads (forward_test.c/run_forward.sh already
 *  does that).
 *
 *  Build:
 *    cc -std=c11 -O1 -ffp-contract=off cradle_live_teach_proof.c \
 *       ../../arch/common/llm/cradle.c ../../arch/common/llm/student.c \
 *       ../../arch/common/llm/llm_shell.c ../../arch/common/llm/gguf.c \
 *       ../../arch/common/llm/quant.c ../../arch/common/llm/pk_parallel.c \
 *       ../../arch/common/llm/tokenizer.c ../../arch/common/llm/forward.c \
 *       ../../arch/common/llm/sample.c \
 *       -lpthread -lm -o cradle_live_teach_proof
 */
#include <stdio.h>
#include <stdint.h>
#include "../../arch/common/llm/student.h"

extern int llm_generate_text(const char *prompt, int max_gen, char *out, int out_cap);

static void emit(const char *s) { fputs(s, stdout); }

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *prompt = (argc > 1) ? argv[1] : "Once upon a time";

    printf("=== cradle_live_teach_proof — CT-2 live-vs-fixture cert (in-proc) ===\n");
    printf("[gen] prompt: \"%s\"\n", prompt);

    static char text[2048];
    int tl = llm_generate_text(prompt, 96, text, (int)sizeof text);
    if (tl <= 0) {
        printf("[gen] rc=%d — no live text (set PKERNEL_LLM_GGUF to a SmolLM2 .gguf)\n", tl);
        printf("RESULT: SKIP (engine unavailable; not this cert's own failure)\n");
        return 0;
    }
    printf("[gen] generated %d bytes: %.*s\n", tl, tl, text);
    printf("\n");

    float live_drop = 0.0f, base_drop = 0.0f;
    int fails = cradle_live_teach_test(emit, (const uint8_t *)text, tl,
                                       &live_drop, &base_drop);
    printf("\n");
    printf("live_drop=%.4f base_drop=%.4f\n", (double)live_drop, (double)base_drop);
    if (fails == 0) {
        printf("RESULT: PASS — the live-generated lesson lowered held-out loss on "
               "its own text well past the fixture-only baseline\n");
        return 0;
    }
    printf("RESULT: NO DIFFERENCE (honest) — see numbers above\n");
    return 1;
}
