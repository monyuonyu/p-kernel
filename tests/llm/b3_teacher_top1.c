/*
 * b3_teacher_top1.c — the kernel's own side of the B3 TEACHER check
 * (ROADMAP B3; design research/b3-data.md section 2).
 *
 * The B3 texts are made by the teacher through PyTorch (b3_gen.py), not by the
 * kernel's forward.c. To say it is the same teacher, both read the same GGUF
 * and the same token ids, and the top-1 next token is compared at 256
 * positions (pass line 95 %, judged by b3_curve_check.py from the
 * "[b3-teacher] match=N of 256" line run_b3_teacher.sh writes).
 *
 * This program feeds the ids one by one through lm_forward and prints
 * "<pos> <argmax id>" per position, the same lines as b3_gen.py --top1.
 *
 *   b3_teacher_top1 <model.gguf> <ids.txt>     exit 0 = printed every position
 */
#include <stdio.h>
#include <stdlib.h>
#include "../../arch/common/llm/gguf.h"
#include "../../arch/common/llm/forward.h"

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s <model.gguf> <ids.txt>\n", argv[0]); return 2; }
    static int ids[4096];
    int n = 0;
    FILE *f = fopen(argv[2], "r");
    if (!f) { perror(argv[2]); return 2; }
    while (n < (int)(sizeof ids / sizeof ids[0]) && fscanf(f, "%d", &ids[n]) == 1) n++;
    fclose(f);

    gguf_file gf;
    int rc = gguf_open(&gf, argv[1]);
    if (rc) { fprintf(stderr, "gguf_open %s: %s\n", argv[1], gguf_strerror(rc)); return 2; }
    lm_model m;
    rc = lm_load(&m, &gf);
    if (rc) { fprintf(stderr, "lm_load: %s\n", lm_strerror(rc)); gguf_close(&gf); return 2; }
    if (n > m.max_ctx) { fprintf(stderr, "%d ids > max_ctx %d\n", n, m.max_ctx); return 2; }
    lm_reset(&m);
    for (int p = 0; p < n; p++) {
        if (ids[p] < 0 || ids[p] >= m.vocab || lm_forward(&m, ids[p]) < 0) {
            fprintf(stderr, "forward failed at %d (id %d)\n", p, ids[p]);
            return 1;
        }
        printf("%d %d\n", p, lm_argmax(&m));
    }
    lm_free(&m);
    gguf_close(&gf);
    return 0;
}
