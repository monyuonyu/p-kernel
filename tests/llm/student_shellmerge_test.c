/*
 *  student_shellmerge_test.c - the shell's `student merge` must not make the
 *  live student worse (research/brain-merge-e3-log.md: its peer came from
 *  another init, 0x5151 against g_student's 0x0BABE, and the plain mean
 *  replaced the live weights with no guard; parents from different inits gave
 *  mean - worse +1.17..+1.55 at ST_LR on the probe's seen seeds).
 *
 *  Links the REAL student_shell.c (#include, like student_nan_test.c), trains
 *  the resident student with the production DMN batches on the fixture, then
 *  runs `student merge` and reads its own line.
 *
 *   [smerge-ran]   the verb ran and said "held-out loss a -> b"
 *   [smerge-held]  b <= a: the held-out loss of the live student does not rise
 *   [smerge-finite] the live weights are finite after the merge
 *
 *  Falsifier (run_shellmerge.sh): -DST_SHELL_MERGE_PLAIN, the old verb
 *  (peer from 0x5151 at 2e-2, plain st_merge_cohort), must turn
 *  [smerge-held] red. Build: -std=c11 -O1 -Wall -Wextra -ffp-contract=off.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../arch/common/llm/student_shell.c"

/* ---------- kernel-seam stubs (as in student_nan_test.c) ---------- */
int pfs_dur_active(void) { return 0; }
int pfs_dur_write(const char *f, const void *d, unsigned n) { (void)f;(void)d;(void)n; return -1; }
int pfs_dur_read (const char *f, void *b, unsigned m) { (void)f;(void)b;(void)m; return -1; }
int st_init_device(st_model *m, uint32_t seed) { return st_init_tier(m, seed, ST_TIER_S); }
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

static char g_said[8192];
static void said(const char *s)
{
    size_t n = strlen(g_said);
    snprintf(g_said + n, sizeof g_said - n, "%s", s);
}
static void tick(void)
{
    int rc = 0, calls = 0;
    do { rc = student_dmn_consolidate(); calls++; } while (rc == 0 && calls < 100000);
}
static int finite_w(const st_model *m)
{
    for (int i = 0; i < m->n_params; i++) {
        uint32_t u;
        memcpy(&u, &m->w[i], sizeof u);
        if ((u & 0x7F800000u) == 0x7F800000u) return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    int ticks = argc > 1 ? atoi(argv[1]) : 6;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (student_ensure(0) != 0) { printf("[result] FAIL (student_ensure)\n"); return 1; }
    for (int t = 0; t < ticks; t++) tick();
    g_said[0] = 0;
    int rc = student_shell_cmd("merge", said);
    const char *p = strstr(g_said, "held-out loss ");
    float a = 0.0f, b = 0.0f;
    int got = p && sscanf(p, "held-out loss %f -> %f", &a, &b) == 2;
    printf("    after %d DMN batches: %s", ticks, p ? p : "(no line)\n");
    CHECK(rc == 0 && got, "[smerge-ran] the verb ran and said its held-out loss");
    CHECK(got && b <= a, "[smerge-held] the merge does not raise the live student's held-out loss");
    CHECK(finite_w(&g_student), "[smerge-finite] the live weights are finite");
    printf("[result] %s (%d pass, %d fail)\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
