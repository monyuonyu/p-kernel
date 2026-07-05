/*
 *  frontier_stub.c — WEAK no-op fallbacks for the Frontier Mouth ABI.
 *
 *  The student_stub.c pattern (dev_capacity.c:8-11), applied to frontier.c.
 *
 *  WHY IT EXISTS / WHY IT IS NOT (yet) LINKED ANYWHERE:
 *  ---------------------------------------------------
 *  frontier.c's STRONG definitions live in the HOSTED LLM tier (boot/linux +
 *  boot/linux_x86_64 LLM_C_SRCS, and the android CMake LLM_STUDENT_SRC). Its
 *  ONLY callers are hosted-only TUs — galaxy.c (the CONSULT verb, hosted-guarded)
 *  and cradle.c (the TEACH ride, hosted-guarded) — NEITHER of which compiles on
 *  bare metal. So NO bare-metal-linked TU references a frontier symbol, and the
 *  bare-metal link needs no frontier object at all. Consequently this stub is
 *  deliberately NOT added to boot/x86 / boot/aarch64: adding an object to the
 *  bare-metal link would risk perturbing .text and BREAK the crown byte-identity
 *  (verified: bare-metal .text stays aarch64 7f3fbda4…/x86 260da329…). Frontier
 *  is a peripheral organ (Body layer) — absent by default, exactly like the
 *  design says (§1.1).
 *
 *  It is provided (compilable, matching signatures) so that the DAY a bare-metal
 *  or Bionic-minimal target ever references a frontier symbol, the link resolves
 *  to a graceful "no mouth" — the same safety student_stub.c gives the resident-
 *  baby ABI. The defs are __attribute__((weak)) so the real frontier.c always
 *  wins on a hosted build (a strong symbol beats a weak one; no duplicate-symbol
 *  error). No headers, no libc, no allocation — safe in a freestanding TU. The
 *  signatures MUST match frontier.h byte-for-byte or the strong override would
 *  silently fail to take effect.
 */

/* Matches frontier.h's fr_frame_fn-taking consult; a target with no frontier
 * organ always degrades: return 0 (no mouth) so the caller streams the student's
 * own voice. Emits NOTHING (never a fabricated citation). */
__attribute__((weak)) int frontier_consult(const char *text, int len,
                          void (*frame)(void *, const char *, int), void *ctx)
{
    (void)text; (void)len; (void)frame; (void)ctx;
    return 0;   /* no mouth on this target */
}

__attribute__((weak)) const char *frontier_degrade_note(void)
{
    return "[consult] no mouth: answering alone";
}

__attribute__((weak)) int  frontier_consent_ok(void)    { return 0; }
__attribute__((weak)) void frontier_consent_grant(void) { }

__attribute__((weak)) const char *frontier_status_line(void)
{
    return "mouth: own voice";
}

/* Matches frontier.h's frontier_teach_ingest. With no frontier organ there is
 * no volunteer teacher path here: refuse (return <0), never silently ingest. */
struct FR_TEACH_HDR_stub;   /* opaque — the weak stub never inspects it */
__attribute__((weak)) int frontier_teach_ingest(const void *hdr,
                          const unsigned char *body, int len)
{
    (void)hdr; (void)body; (void)len;
    return -1;   /* no teach organ -> refuse */
}

__attribute__((weak)) const char *frontier_last_teach_model(void)   { return ""; }
__attribute__((weak)) const char *frontier_last_teach_license(void) { return ""; }
__attribute__((weak)) int         frontier_last_teach_kind(void)    { return 0; }

/* the cert suite is hosted-only; on a target without frontier.c it is a no-op. */
__attribute__((weak)) void frontier_self_test(void (*emit)(const char *))
{
    if (emit) emit("[frontier] no mouth on this target (stub)\r\n");
}
