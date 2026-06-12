/*
 *  sign.h — the node signing layer: ONE Ed25519 keypair per NODE, the
 *  signer-allowlist, and the falsifiable `sign test` suite. Built on the
 *  ed25519.h primitive (RFC 8032, KAT-gated).
 *
 *  THE BOUNDARY (signing.md §0; owner directive
 *  docs/claude-memory/feedback_ark_no_identity_verification.md): a signature
 *  attests "this ARTIFACT (code unit / weight blob / Self-chain entry) was
 *  produced by the holder of this KEY." It NEVER verifies a human. The key
 *  belongs to the NODE, not a person. No profile/handle/identity is signed
 *  anywhere in this layer. Said twice, on purpose.
 *
 *  Trust model (signing.md §3): TOFU (Self layer pins a chain's origin key on
 *  the first valid entry) + an explicit per-node signer-allowlist (selfc /
 *  genome adopt keys). NOT a PKI / CA / web-of-trust.
 *
 *  arch/common discipline: fixed-width types, static buffers, no host libc,
 *  output via sio_send_frame. The per-arch entropy source is provided by
 *  sign_entropy() (arch/linux: /dev/urandom; bare-metal: RDRAND or a loudly-
 *  flagged weak fallback — a weak keygen RNG silently weakens every signature,
 *  so it is made VISIBLE, never silent).
 */
#pragma once
#include "kernel.h"
#include "ed25519.h"

/* The node keypair, generated once at first boot from sign_entropy(). The
 * secret seed NEVER gossips / never enters a kdds or pfs-replicated path; only
 * the 32-byte public key is a publishable content-addressed identity. */
INT  sign_node_key_ensure(void);                 /* idempotent; 1=ok 0=fail   */
const U1 *sign_node_pubkey(void);                /* 32 bytes, or NULL          */
INT  sign_node_keygen_was_strong(void);          /* 1=strong entropy 0=weak    */

/* Sign an artifact (its content-id, or any bytes) with the node's secret key.
 * Deterministic. sig_out is 64 bytes. Returns 1 on success, 0 if no key. */
INT  sign_artifact(const U1 *msg, UW msg_len,
                   U1 sig_out[ED25519_SIGNATURE_LEN]);

/* Verify a detached signature over msg under an explicit public key.
 * Returns 1 iff valid (fail-closed). Pure wrapper over ed25519_verify. */
INT  sign_verify(const U1 *msg, UW msg_len,
                 const U1 sig[ED25519_SIGNATURE_LEN],
                 const U1 pk[ED25519_PUBLIC_KEY_LEN]);

/* The signer-allowlist (selfc / genome adopt, signing.md §3.2). Adoption is
 * the human act; verification is the machine act. Bounded, per-node, local. */
#define SIGN_ALLOWLIST_MAX 16
INT  sign_allow_add(const U1 pk[ED25519_PUBLIC_KEY_LEN]);  /* 1=added/present */
INT  sign_allow_has(const U1 pk[ED25519_PUBLIC_KEY_LEN]);  /* 1=allowlisted   */
void sign_allow_clear(void);                               /* test reset      */

/* entropy provider — implemented per-arch (arch/linux/sign_entropy.c,
 * arch/x86/sign_entropy.c, arch/aarch64/sign_entropy.c). Fills out[0..n);
 * returns 1 = STRONG (OS RNG / RDRAND), 0 = WEAK fallback (flagged loudly). */
IMPORT int sign_entropy(unsigned char *out, int n);

/* The falsifiable acceptance suite. Emits printed evidence then a canonical
 * "[sign-*] PASS/FAIL" line for each of:
 *   [sign-roundtrip]   sign->verify true; one-byte flip false; + RFC 8032 KATs
 *   [sign-selflayer]   a forged-from-genesis chain by an UNPINNED key REJECTED
 *   [sign-unit]        unsigned / wrong-key unit refused; adopted-key runs
 *   [sign-keyrotation] a successor continues a chain under its OWN key, verifiably
 * Wired to CI via the `sign test` shell verb. */
void sign_self_test(void);
