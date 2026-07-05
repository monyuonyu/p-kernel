/*
 *  scaling_cert.h — the [scaling-*] cert suite + requester-side PURE
 *  society-of-minds ensemble aggregation.
 *
 *  Companion to docs/architecture/scaling-law.md (design 2026-07-04). The
 *  thesis under test: "as N grows the mind gets smarter." The design
 *  decomposes "smarter" into four axes that scale DIFFERENTLY, states which
 *  genuinely scale with N, DESIGNS the one missing mechanism (the ownerless
 *  ensemble ask), and specifies a cert whose disease arm proves node-COUNT
 *  alone buys exactly ZERO per-answer quality.
 *
 *  HOSTED-TIER ONLY. The whole translation unit is wrapped in
 *  #ifdef _TK_HOSTED_LIBC_, and it is NOT listed in the bare-metal
 *  Makefiles (boot/x86, boot/aarch64), so the bare-metal kernel .text (the
 *  crown) gains NO symbol and stays byte-identical to its parent. This is
 *  a cert + a set of PURE aggregation functions; v1 does NOT ship the
 *  ensemble into the bare-metal answer path (that would be a deliberate
 *  crown wave — scaling-law.md §7).
 */
#ifndef SCALING_CERT_H
#define SCALING_CERT_H

#ifdef _TK_HOSTED_LIBC_

/* Run every in-process [scaling-*] arm and print the greppable gate lines.
 * Driven from the shell verb `dtr gossip scaling` (gossip_learn.c gl_cmd). */
void scaling_self_test(void);

#endif /* _TK_HOSTED_LIBC_ */
#endif /* SCALING_CERT_H */
