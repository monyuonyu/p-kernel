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
 *  Step ④ (wave-dmn-student-distill): the DMN sleep tick now DRIVES one
 *  bounded distillation round into the resident baby — sleep = consolidation.
 *  student_dmn_consolidate() is the exact symbol dmn.c's idle hook calls after
 *  the existing R3 living-mind consolidation (a SECOND track, not a
 *  replacement). It is a strict NO-OP when there is no persistence and no
 *  resident baby (a node that isn't raising a baby is completely unaffected:
 *  no 30MB arena, no work, no print).
 *
 *  Honesty / scope (what this is NOT):
 *    - The teacher is STILL a SMALL COMMITTED fixture, not in-kernel SmolLM2
 *      token generation. Live-teacher harvesting is step ⑤.
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
 * The teacher fixture (step ⑤ — make the baby's babble richer).
 *
 * A DIVERSE byte-level corpus HARVESTED from SmolLM2-135M via the SS-1 sampler
 * (temperature 0.8, top-k 40, top-p 0.95, light repetition penalty) over two
 * dozen varied short English prompts ("The cat", "Once upon a time", "Water
 * is", "The sky was", "People learn", "Science tells us", "The forest was",
 * "Cooking is", ...), each with its own RNG seed. See tests/llm/
 * student_harvest_diverse.c for the exact harvester and prompt set, and
 * tests/llm/run_diverse_proof.sh for the old-vs-new richness cert.
 *
 * It REPLACES the prior SMALL repetitive corpus ("the cat sat on the mat ...")
 * that came from GREEDY decoding (lm_generate seeded with a bare BOS), which
 * degenerated into a near-constant loop, so the baby distilled from it only
 * ever learned that one repetition. This varied-English corpus gives the baby
 * real, non-degenerate regularity, so it babbles less repetitively (the cert
 * measures: distinct-byte-trigram ratio up, longest byte run + phrase-looping
 * down, held-out loss comparable). The DMN distill + birth-warmup read this
 * constant exactly as before — only the bytes changed.
 *
 * The bytes were cleaned to printable ASCII + single spaces so this stays a
 * well-formed C string literal; a byte-level baby is indifferent to that.
 * ------------------------------------------------------------------------- */
static const char TEACHER_FIXTURE[] =
    "The cat's eyes, and a warm glow emanating from the lamp on her shoulder. "
    "She seemed to be watching me with an intensity that was both mesmerizing "
    "and unsettling at once - like I had been given permission by some mystical "
    "force to explore this unknown realm without warning Once upon a time, I "
    "remember the day my friend Alex and his brother were born in Mexico. My "
    "grandmother used to tell me stories about them growing up during colonial "
    "times when Spanish settlers had come into our neighborhood from across the "
    "border. It was not always easy for them Water is a vital component of our "
    "ecosystem, and it plays an essential role in the reproduction and survival "
    "of many different species. It's not just for birds to eat; they also play "
    "important roles on land as herbivores or omnivores that help maintain soil "
    "fertility through My favorite foods? The sky was dark, and the stars "
    "twinkled like diamonds on a velvet cloak. A lone constellation pierced by "
    "some piercing glint of light an Orion-like figure with its arms extended "
    "behind him as if he were waiting for his prey to follow shortly after "
    "People learn through experience and experimentation. * **Focus on "
    "community-driven initiatives**: Instead of spreading yourself too thin "
    "across the entire country, focus your efforts at local organizations or "
    "schools that share a similar mission with yours (e.g., environmental "
    "conservation groups). In the morning, I wake up and begin to get a little "
    "anxious. The thought of my sister still having issues with this will be "
    "enough anxiety for me...but after that it is off on its own like usual! She "
    "opened the other door and stepped into a room filled with books on various "
    "subjects. \"Thank you for taking me there,\" I said, gesturing towards my own "
    "desk where papers were neatly arranged upon it. My eyes locked onto yours "
    "once more as we sat down Science tells us the following: When we're doing "
    "something that's hard to do, and you have a lot of time on your hands when "
    "everything else is going well... The only way for me not getting anything "
    "done was because I didn't get my break The old house, you can't help but "
    "feel a sense of longing. It was there when I first moved in with my parents "
    "back home on summer break; it seemed like just yesterday we'd been together "
    "as friends all over again the laughter and songs exchanged between A long "
    "time ago. My family has always known me as Emily Wilson, a dedicated young "
    "lady who worked tirelessly for the city's infrastructure in my hometown of "
    "Oakdale. I've seen some changes over the years - new construction projects "
    "and renovations have all but erased our Music can create an engaging "
    "narrative that draws the reader in and keeps them hooked. You could also "
    "experiment with different tones, styles or genres to find what works best "
    "for your story's unique voice and atmosphere.\" 51708942 (3 The river flows "
    "to the sea. This is because water cannot penetrate through a solid, such as "
    "stone or wood that's too close together and would block its flow into it.\" "
    "- I also used \"this\" instead of 'it' in sentences like: Children love to "
    "celebrate the birthday boy's new best friend. They'll meet up at a park, go "
    "for rides and have some fun together on sunny days! - A group of friends "
    "from school are celebrating a big party this weekend - they all brought "
    "their favorite When winter comes to a close, many people retire in their "
    "40s or beyond. Here are some fun and exciting facts about retirement: 1) "
    "**People who retired were twice as likely on average** when they had more "
    "children than those without it ( The best way to ensure a secure password "
    "is not just for passwords themselves but also the content of those "
    "passwords. This will provide strong, unique combinations that won't be "
    "easily guessed by an advanced program or thief with access at any time.\" "
    "History shows that even when a person is healthy, their brain can still "
    "experience some form of stress. The researcher's goal was to create an "
    "experiment where the subjects were allowed time and opportunities for "
    "self-reflection on how they managed stressors in daily life - whether it "
    "could Birds fly Here are some ideas to get you started: 1.";

/* ---------------------------------------------------------------------------
 * The resident baby. Allocated lazily; reused across verbs within one boot.
 * ------------------------------------------------------------------------- */
static st_model g_student;
static int      g_have_student = 0;   /* 1 once g_student is st_init'd        */
static int      g_loaded_from_disk = 0; /* 1 if restored, 0 if fresh st_init  */

/* FLASH-WEAR skip-write state (wave-student-throttle). The DMN tick used to
 * rewrite the full ~22.8MB blob (open+write+fsync+rename+dir-fsync — NOT a
 * content no-op: pfs_dur_write is a raw filename write, the "content-id no-op"
 * only applies to pfs_dag's content-addressed path) every single round, even
 * after the baby plateaued. We now persist a DMN round only when the held-out
 * loss IMPROVED meaningfully since the last DMN save: track the loss at the
 * last save and the high-water adam_t we last persisted. Until the first DMN
 * save, g_dmn_saved==0 forces the next round's write through unconditionally
 * (the g_dmn_saved_loss value is ignored while the flag is clear), so the first
 * improvement always lands. The human `student` verb (student_persist) is
 * intentionally NOT throttled — an explicit "raise the baby" always saves. */
static int      g_dmn_saved = 0;          /* 1 once a DMN round has persisted   */
static float    g_dmn_saved_loss = 0.0f;  /* held-out loss at the last DMN save */
static int      g_dmn_saved_adam_t = 0;   /* adam_t at the last DMN save        */
static unsigned g_dmn_save_count = 0;     /* # of 22.8MB DMN writes (proof/obs)  */

/* How much the held-out loss must drop (nats) below the last DMN-persisted
 * value before we are willing to rewrite the 22.8MB blob again. Smaller than
 * a single round's early gain (~1 nat) but well above float noise, so a baby
 * that is still genuinely learning keeps persisting and a converged one stops.
 * A modest improvement is also forced through once adam_t has advanced by
 * ST_DMN_SAVE_DT steps even if under epsilon, so slow late-stage gains are
 * not lost forever (bounded staleness of the durable checkpoint). */
#define ST_DMN_SAVE_EPS  0.01f
#define ST_DMN_SAVE_DT   200

/* Per-DMN-tick distill geometry (used by both student_persist's baseline math
 * and student_dmn_consolidate). Defined here, above the first use. */
#define ST_DMN_SEQLEN  32
#define ST_DMN_ROUNDS  2     /* tiny per-tick: prove growth, stay responsive */
#define ST_DMN_LR      3e-3f

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

    /* An explicit human checkpoint resets the DMN throttle baseline so a sleep
     * tick right after `student` does not immediately rewrite the just-saved
     * blob; the next DMN write waits for a real improvement past this point. */
    {
        int corpus_n = (int)sizeof(TEACHER_FIXTURE) - 1;
        int total    = corpus_n / ST_DMN_SEQLEN;
        int trainw   = total * 3 / 4; if (trainw < 2) trainw = 2;
        int heldw    = total - trainw; if (heldw < 1) heldw = 1;
        int train_end = trainw * ST_DMN_SEQLEN;
        g_dmn_saved        = 1;
        g_dmn_saved_loss   = heldout_loss(&g_student, ST_DMN_SEQLEN, train_end, heldw);
        g_dmn_saved_adam_t = g_student.adam_t;
    }

    if (emit) {
        snprintf(line, sizeof line,
                 "[baby] saved %ld bytes to %s (durable; survives restart)\r\n",
                 n, STUDENT_DUR_FILE);
        emit(line);
    }
    return 0;
}

/* Cheap probe: is there a saved baby on the durable backend WITHOUT
 * allocating the ~30MB arena? Reads only the first few bytes of the durable
 * file. Returns 1 if a non-empty saved student is present, 0 otherwise (no
 * persistence, absent, or empty). Deliberately does NOT validate magic/dims
 * (st_load does that on the real restore) — this only decides whether it is
 * worth spending the arena to TRY a restore. */
static int student_have_saved(void)
{
    if (!pfs_dur_active()) return 0;
    unsigned char probe[16];
    int n = pfs_dur_read(STUDENT_DUR_FILE, probe, (unsigned)sizeof probe);
    return n > 0;
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

/* Ensure the resident baby exists, ALLOCATING it on first touch. This is the
 * "I am actively raising a baby" path (the `student` verb, or a DMN tick on a
 * node that already has a saved baby): st_init a fresh ~30MB arena, then try to
 * restore saved weights over it. Idempotent after the first call. Returns 0 on
 * success. Callers that must NOT pay the arena cost unless a baby genuinely
 * exists (boot restore, the DMN tick) gate this behind student_have_saved()
 * (or, for the verb, the human's explicit intent). */
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

/* Birth the freshly-born resident baby and persist it so a restart restores it.
 * Caller has already ensured the baby is resident (g_have_student).
 *
 * DEFERRED-WARMUP (wave-note10-boot): the initial distill burst is NO LONGER run
 * synchronously here. It used to run a few sleep_rounds() of distill inline on
 * the boot path, INSIDE the init/usermain task. Two problems surfaced on a slow
 * device (Galaxy Note10+):
 *   1) Scheduler monopoly: T-Kernel is strictly priority-scheduled and the init
 *      task outranks the prio-8 galaxy server task, so the burst STARVED the
 *      galaxy port — the "lighting your star" splash probes that port and hung
 *      until the whole burst finished.
 *   2) Stack: the burst runs at the DEEP boot call depth (every *_init nested),
 *      and st_forward/backward/adam over the ~30MB model needs real stack —
 *      enough to overflow the modest init-task stack on top of that depth.
 * The DMN sleep tick (student_dmn_consolidate) ALREADY distills this exact baby
 * every idle round, from a FRESH low-depth task stack and at the lowest priority
 * (it cannot starve the port). So we birth the baby CHEAPLY (st_init + one
 * durable save) and let it grow asleep — the intended developmental arc, just
 * without blocking the port or risking the boot stack. The very first chat may
 * babble weakly until the first DMN round lands; the model is identical work,
 * only MOVED off the boot critical path. */
static void student_birth_warmup(emit_fn emit)
{
    /* student_persist resets the DMN throttle baseline to this newborn loss, so
     * the first DMN sleep waits for a real improvement before rewriting flash. */
    student_persist(emit);
    if (emit) emit("[baby] newborn — it will babble after its first sleep "
                   "(the DMN grows it; warmup deferred off the boot path)\r\n");
}

/* ---------------------------------------------------------------------------
 * Boot hook: on a node where the ark REMEMBERS (persistence active), make sure
 * there is a baby to talk to and grow.
 *
 *  - If a saved baby exists on disk -> RESTORE it (wake up remembering
 *    yesterday's sleep). Unchanged.
 *  - Else if persistence is active (the app / phone: PKERNEL_PFS_DIR set) and
 *    there is NO saved baby -> BIRTH a fresh one (st_init) and give it a small
 *    initial distill burst, so a brand-new install has a real (babbling)
 *    student for the chat AND something the DMN sleep tick can grow. The app's
 *    whole premise is "a new mind grows from a baby"; without this a fresh
 *    phone install would chat "…" forever and the DMN would never have a baby.
 *  - Else (PFS-less / relay / bare node: persistence INACTIVE) -> TRUE no-op:
 *    allocate nothing, print nothing, no 30MB arena. The step-③ audit nit and
 *    the "30MB on every node" concern are preserved: birth ONLY when the node
 *    actually persists. The flash-wear throttle (student_persist baseline) is
 *    respected — birth writes exactly once.
 * ------------------------------------------------------------------------- */
int student_boot_restore(emit_fn emit)
{
    if (student_have_saved())                  /* remember yesterday's baby   */
        return student_ensure(emit);

    if (!pfs_dur_active()) return 0;           /* PFS-less/bare -> TRUE no-op  */

    /* Persistence active, no saved baby: a fresh ark. Birth one. */
    if (emit) emit("[baby] no saved baby on this persistent node — "
                   "a new mind is born\r\n");
    if (student_ensure(emit) != 0) return -1;  /* st_init the ~30MB arena      */
    student_birth_warmup(emit);                /* babble now, grow asleep      */
    return 0;
}

/* ---------------------------------------------------------------------------
 * DMN sleep tick (step ④, wave-dmn-student-distill): the consolidation track
 * the DMN idle hook (dmn.c dmn_idle_work) drives ON EVERY sleep round, AFTER
 * the existing R3 living-mind consolidation. ONE small bounded distillation
 * round from the teacher fixture into the resident baby, then a durable save —
 * so the node LEARNS while it sleeps and the gain SURVIVES a reboot.
 *
 *   sleep = consolidation: the ownerless student grows during DMN rest.
 *
 * STRICT NO-OP (returns 0, allocates nothing, prints nothing) unless:
 *   - persistence is active (PKERNEL_PFS_DIR set: nowhere to save = pointless
 *     to train, and a memory-only/bare-metal node must be unaffected), AND
 *   - the node is actually raising a baby — either one is already resident in
 *     this boot (g_have_student, e.g. the human ran `student`), or a saved baby
 *     exists on disk (student_have_saved, e.g. boot restored it).
 *
 * So a node that never touched the baby and has no saved baby does NOTHING on
 * its DMN heartbeat (no arena, no round). Cheap per tick: ST_DMN_ROUNDS small
 * rounds over the fixture's train windows (the MECHANISM, not a full train),
 * so the sleep stays responsive.
 *
 * Returns 1 if it ran a round (caller may emit a sleep line / galaxy event),
 * 0 if it was a no-op.  No emit_fn: the DMN owns the sleep narration.
 * (ST_DMN_SEQLEN / ST_DMN_ROUNDS / ST_DMN_LR are defined near the top so
 * student_persist's throttle-baseline math can also see them.)
 * ------------------------------------------------------------------------- */
int student_dmn_consolidate(void)
{
    /* No persistence -> no save target -> nothing to do (and don't allocate). */
    if (!pfs_dur_active()) return 0;

    /* Only act for a node that is genuinely raising a baby. If none is resident
     * yet, but one is saved on disk, adopt it (st_init + restore) — sleeping is
     * exactly when a restored baby should keep growing. Otherwise NO-OP. */
    if (!g_have_student) {
        if (!student_have_saved()) return 0;      /* baby-less node: untouched */
        if (student_ensure(0) != 0) return 0;     /* arena now; quiet on the tick */
    }

    int corpus_n = (int)sizeof(TEACHER_FIXTURE) - 1;
    int total    = corpus_n / ST_DMN_SEQLEN;
    int trainw   = total * 3 / 4; if (trainw < 2) trainw = 2;
    int heldw    = total - trainw; if (heldw < 1) heldw = 1;
    int train_end = trainw * ST_DMN_SEQLEN;

    sleep_rounds(&g_student, ST_DMN_SEQLEN, trainw, ST_DMN_ROUNDS, ST_DMN_LR);

    /* SKIP-WRITE-WHEN-NOT-WORTH-IT (wave-student-throttle): persist the
     * post-sleep state ONLY when the baby actually improved meaningfully since
     * the last DMN save, so a converged/idle baby STOPS rewriting the ~22.8MB
     * blob (and stops fsync'ing flash) every tick. The latest MEANINGFUL
     * weights are always the ones on disk: the last write captured the best
     * loss so far; a later non-improving round has nothing worth persisting,
     * and restart-survival restores that best checkpoint. We still force a
     * write through once adam_t has advanced ST_DMN_SAVE_DT steps past the last
     * save even under epsilon, so slow late gains eventually land (bounded
     * checkpoint staleness). Honest heuristic: a baby that genuinely keeps
     * learning keeps persisting; the wear stops exactly when learning does. */
    float cur = heldout_loss(&g_student, ST_DMN_SEQLEN, train_end, heldw);
    int improved   = !g_dmn_saved || (cur < g_dmn_saved_loss - ST_DMN_SAVE_EPS);
    int dt_elapsed = g_dmn_saved &&
                     (g_student.adam_t - g_dmn_saved_adam_t) >= ST_DMN_SAVE_DT &&
                     cur < g_dmn_saved_loss;     /* never persist a regression */
    int worth_save = improved || dt_elapsed;

    if (worth_save && pfs_dur_active()) {
        size_t need = st_blob_size(&g_student);
        unsigned char *blob = (unsigned char *)malloc(need);
        if (blob) {
            long w = st_save(&g_student, blob, need);
            if (w >= 0 && pfs_dur_write(STUDENT_DUR_FILE, blob, (unsigned)w) == 0) {
                g_dmn_saved        = 1;
                g_dmn_saved_loss   = cur;
                g_dmn_saved_adam_t = g_student.adam_t;
                g_dmn_save_count++;
            }
            free(blob);
        }
    }
    return 1;
}

/* Held-out loss of the resident baby over the fixture's held-out windows, for
 * proofs/observability. Returns chance (logf 256) if there is no baby yet, so
 * a caller sees "no learning" rather than a misleading 0. Pure read. */
float student_dmn_heldout_loss(void)
{
    if (!g_have_student) return st_logf(256.0f);
    int corpus_n  = (int)sizeof(TEACHER_FIXTURE) - 1;
    int total     = corpus_n / ST_DMN_SEQLEN;
    int trainw    = total * 3 / 4; if (trainw < 2) trainw = 2;
    int heldw     = total - trainw; if (heldw < 1) heldw = 1;
    int train_end = trainw * ST_DMN_SEQLEN;
    return heldout_loss(&g_student, ST_DMN_SEQLEN, train_end, heldw);
}

/* Lifetime count of FULL ~22.8MB durable writes the DMN sleep tick has actually
 * performed (wave-student-throttle). Pure read, for the flash-wear proof: with
 * the every-tick code this equalled the number of rounds; with the skip-write
 * heuristic it stops climbing once the baby plateaus. A NEW symbol — no existing
 * student_* signature changes. */
unsigned student_dmn_save_count(void) { return g_dmn_save_count; }

/* ---------------------------------------------------------------------------
 * Chat bridge (step ⑥): the yurikago talks to the RESIDENT baby.
 *
 * galaxy.c's /ws handler calls this with the user's free-text message; we
 * locate the resident student and run st_generate over it, streaming the
 * baby's reply bytes back through `emit_chunk` (galaxy.c frames each chunk as a
 * {"type":"tok"} WS frame and flushes, so the slow baby's text appears
 * progressively).
 *
 * Pure-A (the student generates ON ITS OWN): the SmolLM2 teacher is NOT loaded
 * or consulted here — sleep-time only. This is free-form byte-level generation
 * from whatever the resident baby has learned.
 *
 * RESIDENCY rule (honesty): chat does NOT *birth* a brand-new untrained baby
 * (that would only emit noise and pay a 30MB arena for nothing). It speaks only
 * if a baby is ALREADY resident this boot, OR one is saved on disk (adopt it).
 * Otherwise it returns 0 produced and galaxy streams a gentle placeholder — no
 * crash, no allocation.
 *
 * Bounded/fast (the relay stack lesson): max_gen capped at CHAT_MAXGEN (<=
 * st_generate's own ST_GEN_CAP); the only scratch is one small stack buffer for
 * the prompt bytes, plus the chunk staging the callback owns.
 *
 * Returns the number of reply bytes produced (>=0), or negative on a hard
 * error (galaxy still streams a placeholder + done on <=0).
 * ------------------------------------------------------------------------- */
#define CHAT_MAXGEN  96
#define CHAT_TEMP    0.8f
#define CHAT_TOPK    40

/* the streaming callback galaxy.c gives us: hand it a run of reply bytes. */
typedef void (*chat_emit_fn)(void *ctx, const char *bytes, int n);

/* adapter state: st_generate_stream hands us ONE byte at a time; we forward
 * each immediately to the galaxy chunk sink (galaxy.c flushes per chunk, so a
 * 1-byte chunk == maximally progressive; galaxy may coalesce its own framing). */
struct chat_sink {
    chat_emit_fn emit;
    void        *ctx;
};
static void chat_byte_emit(void *vp, int byte)
{
    struct chat_sink *s = (struct chat_sink *)vp;
    char ch = (char)(unsigned char)byte;
    if (s->emit) s->emit(s->ctx, &ch, 1);
}

int student_chat_generate(const char *intext, int inlen,
                          chat_emit_fn emit_chunk, void *ctx)
{
    /* Speak only for a node actually raising a baby. If none is resident this
     * boot but one is saved on disk, adopt it (sleeping it grew yesterday). */
    if (!g_have_student) {
        if (!student_have_saved()) return 0;          /* no baby -> placeholder */
        if (student_ensure(0) != 0)  return -1;        /* arena now, quiet       */
    }

    if (inlen < 0) inlen = 0;

    /* prompt bytes: take the message TAIL that fits the model's context (the
     * generator caps it too, but keep the bridge's stack buffer bounded). One
     * small fixed buffer — never a network-sized stack array. */
    uint8_t prompt[ST_MAXSEQ];
    int np = 0;
    int start = (inlen > ST_MAXSEQ) ? (inlen - ST_MAXSEQ) : 0;
    for (int i = start; i < inlen && np < ST_MAXSEQ; i++)
        prompt[np++] = (uint8_t)intext[i];

    /* per-message reproducible seed: mix the bytes so different messages diverge
     * but the SAME message replays identically (one-math determinism). */
    uint64_t seed = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < np; i++) seed = seed * 1099511628211ULL + prompt[i] + 1;
    if (!seed) seed = 0xBABEULL;

    struct chat_sink sink = { emit_chunk, ctx };
    uint8_t out[CHAT_MAXGEN];
    int produced = st_generate_stream(&g_student, prompt, np, out, CHAT_MAXGEN,
                                      CHAT_TEMP, CHAT_TOPK, seed,
                                      chat_byte_emit, &sink);
    return produced;
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

    /* ---- SS-3 (special-structure-mind.md §3.2/§8.4): same-tier merge cohort.
     * `student merge` / `baby merge` drives the REAL student-only weight merge
     * (st_merge_cohort) in-process: it builds a SAME-TIER peer, trains it on the
     * shared teacher fixture in a different order, then averages it into the
     * resident baby and prints the accepted-peer count + held-out loss before/
     * after. This is the student's OWN merge — it never calls gl_merge / touches
     * R3's rw[] ([baby-merge-isolation]). HONEST: averaging converges minds on a
     * SHARED objective; divergent-fact recovery is Path-W^2, out of scope. */
    if (p[0]=='m' && p[1]=='e' && p[2]=='r' && p[3]=='g' && p[4]=='e') {
        int sl = 24, rr = 40;
        int cn = (int)sizeof(TEACHER_FIXTURE) - 1;
        int tot = cn / sl, tw = tot * 3 / 4; if (tw < 2) tw = 2;
        int hw = tot - tw; if (hw < 1) hw = 1; int te = tw * sl;

        st_model peer;
        if (st_init_tier(&peer, 0x5151u, g_student.tier) != ST_OK) {
            emit("[baby] merge: peer init OOM\r\n"); return -1;
        }
        /* train the peer on the SAME fixture but rotated order (compatible
         * objective, different SGD path) — reuse the resident sleep loop. */
        sleep_rounds(&peer, sl, tw, rr, 2e-2f);

        float pre = heldout_loss(&g_student, sl, te, hw);
        size_t cap = st_blob_size(&peer);
        unsigned char *blob = (unsigned char *)malloc(cap);
        if (!blob) { st_free(&peer); emit("[baby] merge: blob OOM\r\n"); return -1; }
        long blen = st_save(&peer, blob, cap);
        const void *peers[1] = { blob };
        size_t lens[1] = { (size_t)blen };
        int acc = st_merge_cohort(&g_student, peers, lens, 1);
        float post = heldout_loss(&g_student, sl, te, hw);
        free(blob); st_free(&peer);

        snprintf(line, sizeof line,
                 "[baby] SS-3 cohort merge: tier=%d accepted_peers=%d  "
                 "held-out loss %.4f -> %.4f nats\r\n",
                 g_student.tier, acc, (double)pre, (double)post);
        emit(line);
        emit("[baby] (averaging converges a SHARED objective; divergent-fact "
             "recovery is Path-W^2, out of scope — wave-41)\r\n");
        return acc >= 0 ? 0 : -1;
    }

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
