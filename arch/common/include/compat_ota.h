/*
 *  compat_ota.h — the [signed-ota-gate] accept predicate
 *  (compat-migration-chain-plan.md §4). A node ACCEPTS an update artifact ONLY
 *  if it is correctly SIGNED (the shipped sign_manifest_verify 3-gate AND) AND
 *  its version is a LEGAL SUCCESSOR of the running one (no downgrade, no
 *  body-swap). Reuses sign.c / ed25519.c verbatim; invents NO new crypto.
 *
 *  THE BOUNDARY: the gate attests an ARTIFACT and a KEY, never a human. No
 *  identity/author/handle check is added here (signing.md §0).
 */
#pragma once
#include "kernel.h"
#include "sign.h"          /* SIGN_MANIFEST, sign_manifest_verify (reused)     */
#include "pfs_block.h"     /* PFS_ID_LEN — the recomputed artifact content-id  */

/* rejection reasons — so the caller/cert can print WHICH gate fired. */
#define OTA_ACCEPT            1
#define OTA_REJECT_BADARG     0   /* null arg                                  */
#define OTA_REJECT_SIG       (-1) /* gate 1-3: sign_manifest_verify refused    */
#define OTA_REJECT_DOWNGRADE (-2) /* gate 4: ver <= running / non-successor    */

/* The 4-gate AND (design §4.2). Returns OTA_ACCEPT iff:
 *   (1)(2)(3) sign_manifest_verify(m, actual_id) == 1   — the SHIPPED 3 gates
 *             (recomputed artifact_id match + Ed25519 valid + signer adopted),
 *   (4)       m->artifact_ver is a legal successor of running_ver (strictly
 *             greater — NO downgrade; the version is inside the signed body so
 *             it cannot be forged without the key).
 * actual_id is the caller's pfs_id_compute over the bytes it is about to
 * install. Else returns the OTA_REJECT_* of the gate that fired. fail-closed.
 *
 * Under -DOTA_SKIP_VERIFY the gate is stubbed to OTA_ACCEPT (the load-bearing
 * falsifier): a tampered OTA is then accepted and the cert goes RED. */
INT compat_ota_accept(const SIGN_MANIFEST *m,
                      const U1 actual_id[PFS_ID_LEN],
                      U4 running_ver);

/* human-readable name of an OTA_* result (for cert/diagnostic output). */
const char *compat_ota_reason(INT r);

/* [signed-ota-gate] cert — accept the good OTA; refuse tampered/wrong-key/
 * downgrade with the gate that fired named; -DOTA_SKIP_VERIFY -> RED. Compiled
 * ONLY under -DOTA_GATE_CERT (hosted), so the default kernel + crown are
 * byte-identical. `compat test ota` drives it. */
#if defined(OTA_GATE_CERT) && defined(_TK_HOSTED_LIBC_)
void compat_ota_gate_test(void);
#endif
