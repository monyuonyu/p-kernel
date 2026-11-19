/*
 *  h36_sync_order_test.c — test-holes H36 (audit-196 x5): the ORDER of the
 *  publish's syncfs and its descriptor (research/b5-m-round.md §4, D4).
 *
 *  gl_student_publish writes the student's chunks and index blocks through the
 *  lazy door (pfs_dur_write_lazy: no fsync each), then ONE syncfs
 *  (pfs_dur_sync), then saves the descriptor "st/<node>" that names them. If
 *  the syncfs moved after the descriptor, a power loss could leave a durable
 *  descriptor naming chunks that never reached the disk. W13c/W13d count the
 *  fsyncs and syncfs calls but never their order, so audit-196's break x5
 *  (syncfs after the descriptor) stayed green there.
 *
 *  This test sees the order through the linker (--wrap, set by the runner):
 *    __wrap_pfs_dur_write_lazy  a lazy write that succeeded marks "unsynced"
 *    __wrap_pfs_dur_sync        a syncfs that succeeded clears the mark
 *    __wrap_pfs_dag_save        a save of "st/..." records the mark at that time
 *  It runs in ONE process, SOLO (no network), flat-file durable store
 *  (PKERNEL_PFS_DIR), with pfs_repl_init so the quiet ids take the lazy door.
 *
 *  Checks:
 *    [h36-lazy]       the publish wrote >= 1 block through the lazy door
 *                     (else this test would see nothing; it must not pass empty)
 *    [h36-desc]       publish returned the chunk count, ONE descriptor save
 *    [h36-order]      at the descriptor save, no lazy write is unsynced
 *    [h36-roundtrip]  fetch gives the same bytes back (the test drives the real path)
 *
 *  The product code is unchanged; the falsifier (x5, in the runner) moves the
 *  syncfs after the descriptor and must turn [h36-order] alone red.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int  gl_student_publish(unsigned char node, const void *blob, unsigned int len);
extern int  gl_student_fetch(unsigned char node, void *out, unsigned int cap);
extern int  pfs_durable_restore(void (*emit)(const char *));
extern int  pfs_durable_active(void);
extern void pfs_repl_init(void);

extern int  __real_pfs_dur_write_lazy(const char *fname, const void *data, unsigned len);
extern int  __real_pfs_dur_sync(void);
extern int  __real_pfs_dag_save(const unsigned char *name, unsigned int nlen,
                                const void *buf, unsigned int len);

static int g_unsynced;          /* a lazy write since the last good syncfs */
static int g_lazy_n, g_sync_n;  /* lazy writes / good syncfs calls          */
static int g_desc_n;            /* "st/" descriptor saves                   */
static int g_desc_unsynced;     /* descriptor saves that saw g_unsynced     */
static int g_lazy_before_desc;  /* lazy writes seen before the first save   */
static int g_trace = 1;
static char g_order[64];        /* L(azy run) S(ync) D(esc), compressed     */
static int  g_on;

static void mark(char c)
{
    if (!g_trace || g_on >= (int)sizeof g_order - 1) return;
    if (c == 'L' && g_on > 0 && g_order[g_on - 1] == 'L') return;   /* runs of L = one L */
    g_order[g_on++] = c; g_order[g_on] = 0;
}

int __wrap_pfs_dur_write_lazy(const char *fname, const void *data, unsigned len)
{
    int r = __real_pfs_dur_write_lazy(fname, data, len);
    if (r == 0) { g_unsynced = 1; g_lazy_n++; mark('L'); }
    return r;
}

int __wrap_pfs_dur_sync(void)
{
    int r = __real_pfs_dur_sync();
    if (r == 0) { g_unsynced = 0; g_sync_n++; mark('S'); }
    return r;
}

int __wrap_pfs_dag_save(const unsigned char *name, unsigned int nlen,
                        const void *buf, unsigned int len)
{
    if (nlen >= 3 && name[0] == 's' && name[1] == 't' && name[2] == '/') {
        if (g_desc_n == 0) g_lazy_before_desc = g_lazy_n;
        g_desc_n++;
        if (g_unsynced) g_desc_unsynced++;
        mark('D');
    }
    return __real_pfs_dag_save(name, nlen, buf, len);
}

static int g_pass, g_fail;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

static void emit(const char *s) { fputs(s, stdout); }

#define BLOB_LEN 20000u          /* several chunks (depth 1), all different */

int main(void)
{
    printf("=== H36: the publish's syncfs before its descriptor ===\n");
    pfs_durable_restore(emit);
    if (!pfs_durable_active()) {
        printf("[setup] FAIL durable store not active (set PKERNEL_PFS_DIR)\n");
        return 2;
    }
    pfs_repl_init();             /* sets the lazy hook for quiet ids */

    unsigned char *blob = malloc(BLOB_LEN), *back = malloc(BLOB_LEN);
    if (!blob || !back) { printf("OOM\n"); return 2; }
    unsigned x = 0x36u;
    for (unsigned i = 0; i < BLOB_LEN; i++) { x = x * 1103515245u + 12345u; blob[i] = (unsigned char)(x >> 16); }

    int np = gl_student_publish(5, blob, BLOB_LEN);
    g_trace = 0;
    printf("[h36] publish rc %d, lazy writes %d, good syncfs %d, descriptor saves %d,"
           " unsynced at a save %d, order %s\n",
           np, g_lazy_n, g_sync_n, g_desc_n, g_desc_unsynced, g_order);

    CHECK(g_lazy_before_desc > 0, "[h36-lazy] the publish wrote blocks through the lazy door");
    CHECK(np > 0 && g_desc_n == 1, "[h36-desc] publish returned the chunk count, one descriptor save");
    CHECK(g_desc_n == 1 && g_desc_unsynced == 0,
          "[h36-order] no lazy write was unsynced when the descriptor was saved");

    memset(back, 0, BLOB_LEN);
    int nf = gl_student_fetch(5, back, BLOB_LEN);
    CHECK(nf == (int)BLOB_LEN && memcmp(blob, back, BLOB_LEN) == 0,
          "[h36-roundtrip] fetch gives the same bytes back");

    printf("[h36] %d pass, %d fail\n", g_pass, g_fail);
    free(blob); free(back);
    return g_fail ? 1 : 0;
}
