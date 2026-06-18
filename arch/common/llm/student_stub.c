/*
 *  student_stub.c — WEAK no-op fallbacks for the resident-baby (student) ABI.
 *
 *  WHY THIS FILE EXISTS (build regression fix, wave-student-link-fix):
 *  ------------------------------------------------------------------
 *  arch/common/dmn.c and arch/common/galaxy.c call three functions that the
 *  RESIDENT student exposes:
 *
 *      int   student_dmn_consolidate(void);      (dmn.c idle/sleep hook)
 *      float student_dmn_heldout_loss(void);     (dmn.c proof/observability)
 *      int   student_chat_generate(...);         (galaxy.c /ws chat bridge)
 *
 *  Their STRONG definitions live ONLY in arch/common/llm/student_shell.c, which
 *  is compiled into the HOSTED kernel (boot/linux + boot/linux_x86_64, the
 *  LLM_C_SRCS list — it needs the real system libc + malloc/libm). It is NOT
 *  compiled on bare metal (boot/x86, boot/aarch64: no malloc/libm there) and was
 *  NOT in the Android NDK build (android/.../CMakeLists.txt). But dmn.c (both
 *  bare-metal and Android) and galaxy.c (Android) ARE compiled into those
 *  targets — so the Android .so and the bare-metal kernels failed to LINK with
 *  undefined references to the three student symbols.
 *
 *  This file provides those three symbols as __attribute__((weak)) no-ops, so
 *  every target links. The defs are WEAK: on a HOSTED build, student_shell.c's
 *  STRONG definitions OVERRIDE these — the real resident baby still wins and host
 *  behaviour is unchanged (a strong symbol always beats a weak one of the same
 *  name; there is no duplicate-symbol error). On a target WITHOUT student_shell.c
 *  (bare metal, Android), only these weak no-ops exist and the link resolves.
 *
 *  Behaviour of the no-ops (deliberately the "no resident baby" answer):
 *    - student_dmn_consolidate()  -> 0   (the DMN tick did nothing)
 *    - student_dmn_heldout_loss() -> 0.0 (read-only; no baby to measure)
 *    - student_chat_generate()    -> 0   (emit nothing; galaxy then streams its
 *                                         own gentle "no baby" placeholder)
 *
 *  No headers, no libc, no allocation — safe in a freestanding bare-metal TU.
 *  The signatures here MUST match student_shell.c byte-for-byte (esp. the chat
 *  callback type) or the strong override would silently fail to take effect.
 */

/* Matches student_shell.c's `int student_dmn_consolidate(void)`. */
__attribute__((weak)) int student_dmn_consolidate(void)
{
    return 0;   /* no resident baby on this target -> nothing consolidated */
}

/* Matches student_shell.c's `float student_dmn_heldout_loss(void)`. */
__attribute__((weak)) float student_dmn_heldout_loss(void)
{
    return 0.0f;   /* no baby to measure */
}

/* Matches student_shell.c's
 *   int student_chat_generate(const char *intext, int inlen,
 *                             void (*emit_chunk)(void *ctx, const char *bytes, int n),
 *                             void *ctx);
 * Returns 0 produced and emits nothing; galaxy.c streams a placeholder on <=0. */
__attribute__((weak)) int student_chat_generate(const char *intext, int inlen,
                          void (*emit_chunk)(void *ctx, const char *bytes, int n),
                          void *ctx)
{
    (void)intext; (void)inlen; (void)emit_chunk; (void)ctx;
    return 0;   /* no baby resident -> nothing generated */
}

/* Matches student_shell.c's `unsigned student_dmn_save_count(void)` — the
 * flash-wear throttle counter dmn.c reads to report 22.8MB durable writes.
 * (Added with the throttle wave; bare-metal/Android have no resident baby and
 * thus zero saves.) */
__attribute__((weak)) unsigned student_dmn_save_count(void)
{
    return 0;   /* no baby resident -> no durable saves */
}
