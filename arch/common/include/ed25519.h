/*
 *  ed25519.h — from-scratch Ed25519 (RFC 8032) signature primitive +
 *  the SHA-512 it requires. Zero-dependency, freestanding, fixed-width.
 *
 *  THE BOUNDARY (docs/architecture/signing.md §0, owner directive
 *  docs/claude-memory/feedback_ark_no_identity_verification.md): a
 *  p-kernel signature attests "this ARTIFACT (a content-addressed code
 *  unit, weight blob, or Self-chain entry) was produced by the holder of
 *  this KEY." It NEVER verifies a human. Keys belong to NODES, not people.
 *  Nothing here signs a profile/handle/identity. (Said twice, on purpose.)
 *
 *  PROVENANCE — the field math is NOT invented. The curve25519 field
 *  arithmetic, the Edwards point ops, the sign/verify wrappers and the
 *  SHA-512 below are a VERBATIM transcription of TweetNaCl (version
 *  20140427, by Daniel J. Bernstein, Bernard van Gastel, Wesley Janssen,
 *  Tanja Lange, Peter Schwabe, Sjaak Smetsers — released to the PUBLIC
 *  DOMAIN, https://tweetnacl.cr.yp.to/). TweetNaCl's crypto_sign /
 *  crypto_sign_open / crypto_hash (SHA-512) are reproduced unchanged
 *  except for: (a) renaming the public symbols to ed25519_*; (b) the
 *  fixed-width typedefs below in place of <stdint.h> (same widths); and
 *  (c) dropping randombytes (Ed25519 keygen entropy is the caller's job —
 *  see ed25519_keypair_from_seed). NO field-math "optimization" was made.
 *  This lets an auditor diff line-against-tweetnacl.c.
 *
 *  CORRECTNESS GATE: ed25519_self_test() runs the RFC 8032 §7.1
 *  known-answer vectors (sign + verify, byte-exact) UNCONDITIONALLY on
 *  every build, exactly as sha256_self_test() gates the SHA-256 primitive.
 *  If a KAT fails the transcription is wrong; the slice fails closed.
 *
 *  CONSTANT-TIME: NOT claimed for v1 (signing.md §6.2). Verify handles only
 *  public data. The sign path is not adversary-timed on a single-user node;
 *  multi-tenant constant-time hardening is a flagged follow-up.
 *
 *  Uses only plain unsigned types (no <stdint.h>) — same rationale as
 *  relay/sha256.h: safe to mix with TUs carrying the T-Kernel stdint shadow.
 */
#ifndef PKERNEL_ED25519_H
#define PKERNEL_ED25519_H

#include <stddef.h>

#define ED25519_PUBLIC_KEY_LEN  32   /* the node's published identity     */
#define ED25519_SECRET_KEY_LEN  64   /* TweetNaCl sk = seed(32) || pk(32)  */
#define ED25519_SEED_LEN        32   /* keygen entropy input               */
#define ED25519_SIGNATURE_LEN   64   /* R(32) || S(32)                     */
#define ED25519_SHA512_LEN      64

/* SHA-512 one-shot (the hash Ed25519 is built on; transcribed from
 * TweetNaCl crypto_hash). out is 64 bytes. */
void ed25519_sha512(const void *data, size_t len,
                    unsigned char out[ED25519_SHA512_LEN]);

/* Derive a keypair from a 32-byte seed (the secret seed). The seed is the
 * ONLY entropy Ed25519 needs and only at keygen — signing is deterministic
 * (RFC 8032), so there is no per-signature RNG. sk_out = seed || pk_out
 * (TweetNaCl secret-key layout); pk_out is the 32-byte public identity. */
void ed25519_keypair_from_seed(const unsigned char seed[ED25519_SEED_LEN],
                               unsigned char pk_out[ED25519_PUBLIC_KEY_LEN],
                               unsigned char sk_out[ED25519_SECRET_KEY_LEN]);

/* Detached sign: sig_out = Ed25519(sk, msg). Deterministic. Returns 1 on
 * success, 0 (fail-CLOSED) if msg_len exceeds the bound — in which case NO
 * signature is written, symmetric with ed25519_verify's oversize rejection.
 * Never truncates-and-signs (SEC-SIGN-TRUNC). */
int  ed25519_sign(unsigned char sig_out[ED25519_SIGNATURE_LEN],
                  const unsigned char *msg, size_t msg_len,
                  const unsigned char sk[ED25519_SECRET_KEY_LEN]);

/* Detached verify. Returns 1 iff sig is a valid Ed25519 signature of msg
 * under pk, else 0 (fail-closed). */
int  ed25519_verify(const unsigned char sig[ED25519_SIGNATURE_LEN],
                    const unsigned char *msg, size_t msg_len,
                    const unsigned char pk[ED25519_PUBLIC_KEY_LEN]);

/* Returns 0 if ALL built-in KATs pass (SHA-512 NIST + RFC 8032 §7.1
 * Ed25519 sign/verify vectors), non-zero (the failing vector id) on any
 * mismatch. Cheap; runs unconditionally every build like sha256_self_test. */
int  ed25519_self_test(void);

#endif
