/*
 *  student_restore_test.c - [dmn-restore] B4 remainder (research/b4-step3-result.md,
 *  the last section): when the live weights of the resident baby drift past
 *  the saved best by more than ST_DMN_RESTORE_MARGIN on the held-out windows,
 *  the DMN tick reads the saved best back, if it is better than live now.
 *
 *  Links the REAL student_shell.c (#include, like student_ema_test.c); the
 *  durable backend is one in-memory file (write keeps it, read returns it).
 *
 *   [restore-none]   ordinary ticks: no read-back (live is not the saved blob
 *                    after a tick that trained and did not save)
 *   [restore-back]   live spoiled with noise: after one tick, live w == the
 *                    saved blob's w bit for bit, Adam restarted (mu = vu = 0,
 *                    adam_t = 0), and the next save is not forced by DT
 *   [restore-gated]  the file holds a worse model than live (a garbage model,
 *                    the baseline loss set low so the margin is crossed):
 *                    live is not replaced
 *   [restore-noread] the file cannot be read: live is not replaced, no crash
 *   [restore-rest]   after a read-back the same corpus starts no batch (no
 *                    training, save or read); a writer (student_consol_abort)
 *                    ends the rest; a lesson ends the rest
 *  FALSIFIERS: -DST_DMN_RESTORE_DISABLE: [restore-back] goes RED;
 *              -DST_DMN_REST_DISABLE: [restore-rest] goes RED;
 *              -DST_DMN_REST_ABORT_KEEP: [restore-rest] writer goes RED.
 *
 *  Build: run_restore.sh.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>
#include "../../arch/common/llm/student_shell.c"
#ifdef ST_DMN_RESTORE_MARGIN
#define HAVE_READ_BACK 1
#else                           /* a tree without the read-back (to show the test red there) */
#define ST_DMN_RESTORE_MARGIN 0.10f
#endif

/* ---------- kernel-seam stubs (as in student_ema_test.c) ---------- */
static unsigned char *g_file = NULL;
static unsigned       g_file_len = 0;
static unsigned       g_writes = 0;
static int            g_read_ok = 1;
int pfs_dur_active(void) { return 1; }
int pfs_dur_write(const char *f, const void *d, unsigned n)
{
    (void)f;
    unsigned char *b = (unsigned char *)realloc(g_file, n);
    if (!b) return -1;
    memcpy(b, d, n); g_file = b; g_file_len = n; g_writes++;
    return 0;
}
int pfs_dur_read(const char *f, void *b, unsigned m)
{
    (void)f;
    if (!g_read_ok || !g_file) return -1;
    unsigned n = g_file_len < m ? g_file_len : m;
    memcpy(b, g_file, n);
    return (int)n;
}
int st_init_device(st_model *m, uint32_t seed) { return st_init(m, seed); }
int  gguf_open (gguf_file *gf, const char *p) { (void)gf;(void)p; return -1; }
void gguf_close(gguf_file *gf)                { (void)gf; }
int  lm_load   (lm_model *m, const gguf_file *gf) { (void)m;(void)gf; return -1; }
void lm_free   (lm_model *m)                  { (void)m; }
void     ss6_live_install(void *m)   { (void)m; }
void     ss6_live_uninstall(void)    { }
void     ss6_live_set_enabled(int o) { (void)o; }
unsigned ss6_live_req_sent(void)     { return 0; }
unsigned ss6_live_req_served(void)   { return 0; }

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

static void tick(void)
{
    int rc, calls = 0;
    do { rc = student_dmn_consolidate(); calls++; } while (rc == 0 && calls < 100000);
}
static float live_held(int train_end, int heldw)
{
    return heldout_loss(&g_student, ST_DMN_SEQLEN, train_end, heldw);
}
/* w of the model in the file (NULL if it does not load) */
static float *file_w(st_model *tmp)
{
    if (st_load(tmp, g_file, g_file_len) != ST_OK) return NULL;
    return tmp->w;
}
static void spoil(st_model *m, uint32_t s, float amp)
{
    for (int i = 0; i < m->n_params; i++) {
        s = s * 1664525u + 1013904223u;
        m->w[i] += amp * ((float)(s >> 8) / 16777216.0f - 0.5f);
    }
}
static int all_zero(const float *a, int n)
{
    for (int i = 0; i < n; i++) if (a[i] != 0.0f) return 0;
    return 1;
}

int main(int argc, char **argv)
{
    int only_back = argc > 1 && !strcmp(argv[1], "back");   /* the falsifier's run: 1-2 only */
    setvbuf(stdout, NULL, _IOLBF, 0);
    int corpus_n  = (int)sizeof(TEACHER_FIXTURE) - 1;
    st_split sp   = st_held_split(corpus_n, ST_DMN_SEQLEN);
    int train_end = sp.trainw * ST_DMN_SEQLEN, heldw = sp.heldw;
#ifdef ST_DMN_RESTORE_DISABLE
    printf("[restore] *** built with -DST_DMN_RESTORE_DISABLE (falsifier: never read back) ***\n");
#endif
    st_model tmp;
    if (st_init(&tmp, STUDENT_SEED) != ST_OK) { printf("[restore] st_init OOM\n"); return 2; }
    g_have_student = 0; g_consol_active = 0; g_consol_idx = 0;
    if (student_ensure(0) != 0) { printf("[restore] student_ensure OOM\n"); return 2; }
    size_t nb = (size_t)g_student.n_params * sizeof(float);

    /* "drift T": a reading, not a check - T ordinary ticks on the product's
     * default, live and the saved loss after each (audit-130's off.log form,
     * with a durable file that can be read back) */
    if (argc > 2 && !strcmp(argv[1], "drift")) {
        int T = atoi(argv[2]);
        for (int t = 1; t <= T; t++) {
            unsigned before = g_writes;
            tick();
            printf("[drift] tick=%d live=%.4f saved=%.4f wrote=%d", t,
                   (double)live_held(train_end, heldw), (double)g_dmn_saved_loss, g_writes != before);
#ifdef HAVE_READ_BACK
            printf(" read-backs=%u", g_dmn_restores);
#endif
            printf("\n");
        }
        st_free(&tmp); free(g_file);
        return 0;
    }

    /* 1. ordinary ticks */
    int none = 1;
    for (int t = 1; t <= 3; t++) {
        unsigned before = g_writes;
        tick();
        float *fw = file_w(&tmp);
        int same = fw && memcmp(fw, g_student.w, nb) == 0;
        printf("[restore] tick=%d live=%.4f saved=%.4f wrote=%d live==file %d\n", t,
               (double)live_held(train_end, heldw), (double)g_dmn_saved_loss, g_writes != before, same);
        if (g_writes == before && same) none = 0;    /* trained, did not save, yet is the file */
    }
#ifdef HAVE_READ_BACK
    /* early ticks all save (live == file then), so the counter is the check */
    none = none && g_dmn_restores == 0;
#endif
    CHECK(g_writes > 0 && none, "[restore-none] ordinary ticks do not read the file back");

    /* 2. live spoiled: one tick reads the best back */
    float saved = g_dmn_saved_loss;
    spoil(&g_student, 7u, 0.4f);
    float spoiled = live_held(train_end, heldw);
    unsigned before = g_writes;
    tick();
    float *fw = file_w(&tmp);
    int back = fw && memcmp(fw, g_student.w, nb) == 0;
    int adam0 = all_zero(g_student.mu, g_student.n_params) && all_zero(g_student.vu, g_student.n_params)
                && g_student.adam_t == 0;
    float after = live_held(train_end, heldw);
    printf("[restore] spoiled live %.4f (saved %.4f, margin %.2f) -> after the tick %.4f, wrote=%d"
           " back=%d adam0=%d adam_t=%d\n", (double)spoiled, (double)saved, (double)ST_DMN_RESTORE_MARGIN,
           (double)after, g_writes != before, back, adam0, g_student.adam_t);
    CHECK(spoiled > saved + ST_DMN_RESTORE_MARGIN, "[restore-back] the spoil crosses the margin (setup)");
    CHECK(g_writes == before && back && adam0, "[restore-back] live == the saved best, Adam restarted");
#ifdef HAVE_READ_BACK
    printf("[restore] read-backs so far %u\n", g_dmn_restores);
    CHECK(g_dmn_restores == 1, "[restore-back] exactly one read-back");
#endif
    /* the next tick: with the rest (feat/b4-rest) it starts no batch; under
     * -DST_DMN_REST_DISABLE it trains from the best. Either way DT must not
     * force a write of a regression */
    before = g_writes;
    tick();
    float t2 = live_held(train_end, heldw);
    printf("[restore] next tick live %.4f wrote=%d saved %.4f\n", (double)t2, g_writes != before,
           (double)g_dmn_saved_loss);
    CHECK(g_writes == before || t2 < saved, "[restore-back] the next write is an improvement or none");
#ifdef HAVE_READ_BACK
    /* 2b. rest after the read-back: on the same corpus no batch starts (no
     * training, no save, no read); a lesson (a changed corpus) ends the rest.
     * The tick above already rested; this one must too. */
    {
        int ta = g_student.adam_t; unsigned rb = g_dmn_restores, w0 = g_writes, r0 = g_dmn_rested;
        tick();
        printf("[restore] rest: adam_t %d -> %d, read-backs %u -> %u, writes %u -> %u, rested %u -> %u\n",
               ta, g_student.adam_t, rb, g_dmn_restores, w0, g_writes, r0, g_dmn_rested);
        CHECK(g_student.adam_t == 0 && g_student.adam_t == ta && g_dmn_restores == rb && g_writes == w0 &&
              g_dmn_rested > r0, "[restore-rest] same corpus after a read-back: no training, no save, no read");
        /* a writer of g_student (merge, retrain, restore: each calls
         * student_consol_abort first) ends the rest: here it spoils live past
         * the margin, and the next tick must read the best back (audit-132: on
         * 69272b19 the DMN kept resting with live 5.5876, saved 2.5173) */
        student_consol_abort();
        spoil(&g_student, 9u, 0.4f);
        rb = g_dmn_restores; r0 = g_dmn_rested;
        float sp2 = live_held(train_end, heldw);
        tick();
        float *fw2 = file_w(&tmp);
        int back2 = fw2 && memcmp(fw2, g_student.w, nb) == 0;
        printf("[restore] writer then spoil: live %.4f -> %.4f (saved %.4f), read-backs %u -> %u, rested %u -> %u,"
               " back=%d\n", (double)sp2, (double)live_held(train_end, heldw), (double)g_dmn_saved_loss,
               rb, g_dmn_restores, r0, g_dmn_rested, back2);
        CHECK(g_dmn_restores == rb + 1 && back2,
              "[restore-rest] a writer ends the rest: a spoiled live is read back");
        static uint8_t les[4096];
        int fl = (int)sizeof(TEACHER_FIXTURE) - 1, ln = fl < (int)sizeof les ? fl : (int)sizeof les;
        memcpy(les, TEACHER_FIXTURE, (size_t)ln);
        les[0] ^= 1;
        cradle_set_enabled(1);
        int got = cradle_lesson_ingest(les, ln);
        int rl = 0; const uint8_t *src = cradle_window_src(&rl);
        ta = g_student.adam_t;
        tick();
        printf("[restore] lesson of %d bytes: ingest %d, ring %d, adam_t %d -> %d, resting %d\n",
               ln, got, src ? rl : 0, ta, g_student.adam_t, g_dmn_rest);
        CHECK(got > 0 && src && g_student.adam_t > ta && !g_dmn_rest,
              "[restore-rest] a changed corpus (a lesson) ends the rest");
    }
#endif
    if (only_back) goto out;   /* the falsifiers stop here */

    /* 3. the file holds a worse model than live */
    {
        st_model g;
        if (st_init(&g, STUDENT_SEED ^ 0xBADu) != ST_OK) return 2;
        for (int i = 0; i < g.n_params; i++) g.w[i] *= 8.0f;
        size_t cap = st_blob_size(&g);
        unsigned char *b = (unsigned char *)malloc(cap);
        long n = b ? st_save(&g, b, cap) : -1;
        if (n < 0) return 2;
        free(g_file); g_file = b; g_file_len = (unsigned)n;
        g_dmn_saved_loss = 0.0f;               /* any live is past the margin now */
        tick();
        int kept = memcmp(g.w, g_student.w, nb) != 0 && isfinite(live_held(train_end, heldw));
        printf("[restore] garbage in the file: live %.4f, replaced %d\n",
               (double)live_held(train_end, heldw), !kept);
        CHECK(kept, "[restore-gated] a worse file does not replace live");
        st_free(&g);
    }

    /* 4. the file cannot be read */
    {
        g_read_ok = 0;
        g_dmn_saved_loss = 0.0f;
        int t0 = g_student.adam_t;
        tick();
        int finite = isfinite(live_held(train_end, heldw));
        CHECK(finite && g_student.adam_t > t0, "[restore-noread] no file: live goes on training");
    }
out:
    st_free(&tmp); free(g_file);
    printf("[result] %s (%d pass, %d fail)\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
