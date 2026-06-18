/*
 *  student_shell.c — the RESIDENT Cradle baby: an in-kernel, PERSISTED student.
 *
 *  Step ③ of "p-kernel's living chat" (docs/architecture/native-student.md
 *  §B.6/§B.7, living-mind.md). The distill MECHANISM is already proven on the
 *  host (tests/llm/distill_proof.c: held-out loss 5.53 -> 1.79 nats vs a
 *  scrambled control at 0). This file brings the baby INTO the running kernel
 *  and makes what it learns SURVIVE A REBOOT ("the ark remembers").
 *
 *  Three moving parts:
 *    1) a RESIDENT st_model (one per node), lazily st_init'd or — if a saved
 *       student exists on the durable backend — RESTORED from it on first use;
 *    2) a `student`/`baby` shell verb that runs ONE bounded distillation round
 *       from a small PRE-HARVESTED teacher-byte fixture (the SmolLM2 teacher is
 *       NOT loaded in-kernel here — that is step ④, the live DMN tick), prints
 *       held-out loss before/after, and SAVES the baby durably;
 *    3) durable save/load via the kernel's pfs_durable seam (temp+rename+fsync),
 *       to a stable filename under $PKERNEL_PFS_DIR — the SAME backend R3's own
 *       weights persist through (r3_weights_persist).
 *
 *  Tier / build (mirrors llm_shell.c): host/Android tier ("身体"). Built with
 *  the REAL system libc and the LLM_CFLAGS recipe (-O1 -ffp-contract=off, no
 *  kernel -I so the system <stdint.h> wins). The kernel side (usermain.c) calls
 *  exactly ONE function — student_shell_cmd() — passing a plain line-printer;
 *  it never includes a student header. OFF the boot path: the resident baby is
 *  only ever allocated when the verb (or boot-restore) first touches it, so a
 *  student-less boot is completely unaffected.
 *
 *  Honesty / scope (what this is NOT):
 *    - The teacher is a SMALL COMMITTED fixture, not in-kernel SmolLM2 token
 *      generation. Live-teacher harvesting is step ④.
 *    - Distillation is driven by the HUMAN verb, NOT yet by the live DMN sleep
 *      tick. Wiring the DMN heartbeat to call this is also step ④.
 *    - Single node. No weight diffusion / merge across the fleet (NS-2+).
 */
#include "student.h"
#include <stdlib.h>     /* malloc / free                          */
#include <string.h>     /* memcpy / strlen                        */
#include <stdio.h>      /* snprintf (output formatting only)      */

typedef void (*emit_fn)(const char *);

/* ---- durable seam (arch/linux/pfs_durable.c; linked into the hosted
 * binary via ARCH_SHARED_C_SRCS). Plain C symbols, declared here so this
 * LLM-tier TU need not see the kernel headers. No-op-safe: pfs_dur_active()
 * is 0 when $PKERNEL_PFS_DIR is unset, so a memory-only node still runs. */
extern int  pfs_dur_active(void);
extern int  pfs_dur_write(const char *fname, const void *data, unsigned len);
extern int  pfs_dur_read (const char *fname, void *buf, unsigned maxlen);

/* Stable on-disk name for the resident baby's weights (lives alongside R3's
 * "r3_weights.bin" under $PKERNEL_PFS_DIR). */
#define STUDENT_DUR_FILE "ns1_student.bin"

/* ---------------------------------------------------------------------------
 * The teacher fixture.
 *
 * A SMALL, structured, byte-level corpus — NOT random, so the baby has real
 * regularity to learn (a fast, bounded mechanism proof), but honestly NOT a
 * fresh SmolLM2 harvest. This is exactly the structured stream tests/llm/
 * student_test.c falls back to when no GGUF teacher is present, so the verb's
 * numbers are directly comparable to the committed cert. Step ④ replaces this
 * with bytes the in-kernel teacher actually generates.
 * ------------------------------------------------------------------------- */
static const char TEACHER_FIXTURE[] =
    "the cat sat on the mat. the dog ran in the sun. "
    "she said the sea is blue and the sky is blue too. "
    "the cat and the dog ran to the sea and sat on the sand. "
    "the sun set and the sky was red. the cat slept on the mat again. "
    "the dog ran on the sand and the cat sat in the sun by the sea. ";

/* ---------------------------------------------------------------------------
 * The resident baby. Allocated lazily; reused across verbs within one boot.
 * ------------------------------------------------------------------------- */
static st_model g_student;
static int      g_have_student = 0;   /* 1 once g_student is st_init'd        */
static int      g_loaded_from_disk = 0; /* 1 if restored, 0 if fresh st_init  */

#define STUDENT_SEED 0x0BABEu          /* same seed distill_proof uses         */

/* corpus windowing (identical math to distill_proof.c) */
static void window(uint8_t *dst, int off, int len)
{
    int n = (int)sizeof(TEACHER_FIXTURE) - 1;
    for (int i = 0; i < len; i++) dst[i] = (uint8_t)TEACHER_FIXTURE[(off + i) % n];
}

/* mean held-out loss over `count` windows starting at byte `train_end` */
static float heldout_loss(st_model *m, int seqlen, int train_end, int count)
{
    uint8_t buf[ST_MAXSEQ];
    double s = 0.0; int got = 0;
    for (int w = 0; w < count; w++) {
        window(buf, train_end + w * seqlen, seqlen);
        int np = 0;
        float l = st_eval_loss(m, buf, seqlen, &np);
        if (np) { s += l; got++; }
    }
    return got ? (float)(s / got) : 0.0f;
}

/* K bounded sleep rounds over the train windows (real teacher bytes). */
static void sleep_rounds(st_model *m, int seqlen, int train_windows,
                         int rounds, float lr)
{
    uint8_t buf[ST_MAXSEQ];
    float *logits = (float *)malloc((size_t)seqlen * ST_VOCAB * sizeof(float));
    if (!logits) return;
    for (int r = 0; r < rounds; r++) {
        for (int w = 0; w < train_windows; w++) {
            window(buf, w * seqlen, seqlen);
            st_zero_grad(m);
            st_forward(m, buf, seqlen, logits);
            st_backward(m, buf, seqlen);
            st_adam_step(m, lr);
        }
    }
    free(logits);
}

/* ---------------------------------------------------------------------------
 * Durable save / load of the resident baby.
 * ------------------------------------------------------------------------- */

/* Save g_student to the durable backend. Returns 0 on success, -1 on a
 * durable-write failure, 1 if persistence is disabled (memory-only node). */
static int student_persist(emit_fn emit)
{
    char line[160];
    if (!pfs_dur_active()) {
        if (emit) emit("[baby] persistence OFF (set PKERNEL_PFS_DIR) — "
                       "weights stay memory-only this boot\r\n");
        return 1;
    }
    size_t need = st_blob_size(&g_student);
    unsigned char *blob = (unsigned char *)malloc(need);
    if (!blob) { if (emit) emit("[baby] save OOM\r\n"); return -1; }

    long n = st_save(&g_student, blob, need);
    if (n < 0) { free(blob); if (emit) emit("[baby] st_save failed\r\n"); return -1; }

    int rc = pfs_dur_write(STUDENT_DUR_FILE, blob, (unsigned)n);
    free(blob);
    if (rc != 0) { if (emit) emit("[baby] durable write FAILED\r\n"); return -1; }

    if (emit) {
        snprintf(line, sizeof line,
                 "[baby] saved %ld bytes to %s (durable; survives restart)\r\n",
                 n, STUDENT_DUR_FILE);
        emit(line);
    }
    return 0;
}

/* Try to RESTORE g_student from disk. Caller has already st_init'd g_student
 * (so the arena exists). Returns 1 on a successful load, 0 if absent / disabled
 * / rejected (caller keeps the fresh init). */
static int student_restore(emit_fn emit)
{
    if (!pfs_dur_active()) return 0;
    size_t need = st_blob_size(&g_student);
    unsigned char *blob = (unsigned char *)malloc(need);
    if (!blob) return 0;

    int n = pfs_dur_read(STUDENT_DUR_FILE, blob, (unsigned)need);
    if (n < 0) { free(blob); return 0; }                 /* absent: first boot */
    if (n != (int)need) {
        free(blob);
        if (emit) emit("[baby] persisted weights truncated -> fresh st_init\r\n");
        return 0;
    }
    int rc = st_load(&g_student, blob, (size_t)n);
    free(blob);
    if (rc != ST_OK) {
        if (emit) emit("[baby] persisted weights rejected (build/dim mismatch) "
                       "-> fresh st_init\r\n");
        return 0;
    }
    return 1;
}

/* Ensure the resident baby exists. On first touch: st_init a fresh baby, then
 * try to restore saved weights over it. Idempotent. Returns 0 on success. */
static int student_ensure(emit_fn emit)
{
    if (g_have_student) return 0;
    if (st_init(&g_student, STUDENT_SEED) != ST_OK) {
        if (emit) emit("[baby] st_init OOM\r\n");
        return -1;
    }
    g_have_student = 1;
    g_loaded_from_disk = student_restore(emit);
    if (emit) {
        emit(g_loaded_from_disk
             ? "[baby] resident student RESTORED from durable storage "
               "(it remembers)\r\n"
             : "[baby] resident student initialised FRESH (no saved weights)\r\n");
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Boot hook: restore-or-init the resident baby once at startup, so a node that
 * slept yesterday wakes up remembering. Off the critical boot path otherwise.
 * Safe no-op on a node without persistence (stays a lazy fresh init).
 * ------------------------------------------------------------------------- */
int student_boot_restore(emit_fn emit)
{
    return student_ensure(emit);
}

/* ---------------------------------------------------------------------------
 * The `student` / `baby` shell verb.
 *
 *   student                         one round @ defaults; print + save
 *   student <rounds> <lr> <seqlen>  bounded custom round; print + save
 *   student loss                    just report current held-out loss (no train)
 *
 * Reports held-out loss BEFORE and AFTER the round and the resident baby's
 * provenance (fresh vs restored). Always saves after a real round so the gain
 * survives the next reboot.
 * ------------------------------------------------------------------------- */
int student_shell_cmd(const char *args, emit_fn emit)
{
    char line[200];
    const char *p = args ? args : "";
    while (*p == ' ' || *p == '\t') p++;

    if (student_ensure(emit) != 0) return -1;

    int seqlen = 32, rounds = 8;
    float lr = 3e-3f;
    int loss_only = 0;

    if (p[0] == 'l' && p[1] == 'o' && p[2] == 's' && p[3] == 's') {
        loss_only = 1;
    } else if (*p) {
        /* parse "<rounds> <lr> <seqlen>" (any prefix; missing fields keep
         * defaults). strtod/strtol stay on this host-libc side of the seam. */
        char *end = NULL;
        long r = strtol(p, &end, 10);
        if (end != p && r > 0) { rounds = (int)r; p = end; }
        while (*p == ' ' || *p == '\t') p++;
        if (*p) {
            double v = strtod(p, &end);
            if (end != p && v > 0.0) { lr = (float)v; p = end; }
            while (*p == ' ' || *p == '\t') p++;
            if (*p) {
                long s = strtol(p, &end, 10);
                if (end != p && s >= 2) seqlen = (int)s;
            }
        }
    }
    if (seqlen < 2) seqlen = 2;
    if (seqlen > ST_MAXSEQ) seqlen = ST_MAXSEQ;
    if (rounds > 64) rounds = 64;           /* keep the shell responsive */

    int corpus_n = (int)sizeof(TEACHER_FIXTURE) - 1;
    int total    = corpus_n / seqlen;
    int trainw   = total * 3 / 4; if (trainw < 2) trainw = 2;
    int heldw    = total - trainw; if (heldw < 1) heldw = 1;
    int train_end = trainw * seqlen;
    float chance = st_logf(256.0f);

    snprintf(line, sizeof line,
             "[baby] resident=%s  corpus=%dB seqlen=%d train=%dwin held=%dwin  "
             "chance=%.4f nats\r\n",
             g_loaded_from_disk ? "RESTORED" : "fresh",
             corpus_n, seqlen, trainw, heldw, (double)chance);
    emit(line);

    float pre = heldout_loss(&g_student, seqlen, train_end, heldw);
    snprintf(line, sizeof line, "[baby] held-out loss (before) = %.4f nats\r\n",
             (double)pre);
    emit(line);

    if (loss_only) {
        snprintf(line, sizeof line,
                 "[baby] (loss-only; no training, no save)\r\n");
        emit(line);
        return 0;
    }

    snprintf(line, sizeof line,
             "[baby] distilling %d round(s) lr=%.4f from teacher fixture ...\r\n",
             rounds, (double)lr);
    emit(line);
    sleep_rounds(&g_student, seqlen, trainw, rounds, lr);

    float post = heldout_loss(&g_student, seqlen, train_end, heldw);
    snprintf(line, sizeof line,
             "[baby] held-out loss (after)  = %.4f nats  (drop %.4f, %.1f%% of chance)\r\n",
             (double)post, (double)(pre - post),
             (double)(100.0f * (pre - post) / chance));
    emit(line);

    student_persist(emit);
    return 0;
}
