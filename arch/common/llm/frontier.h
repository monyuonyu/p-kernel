/*
 *  frontier.h — Frontier Mouth: the ownerless mind's OPTIONAL socket onto
 *               stronger brains. Shared cross-TU contracts ONLY.
 *
 *  See docs/architecture/frontier_mouth_design.md. Two channels, one tier:
 *
 *    CONSULT — a LABELED borrowed voice. The human (through the mind's UI)
 *              talks to a frontier model; every frontier byte reaches the
 *              human inside a `src:"frontier"` frame that names the model.
 *              API output is SPOKEN, never eaten — no write-edge into the
 *              student's self (§1.5). Unplug the socket ⇒ the self is
 *              byte-identically what it grew to be.
 *
 *    TEACH   — OPEN-LICENSE teachers only. A volunteer's local model text is
 *              digested via the existing cradle_lesson_ingest → the student's
 *              OWN backward pass. The license line is enforced by the enum
 *              below: there is deliberately NO FRONTIER_API teacher kind.
 *
 *  This header is the ONLY shared surface between frontier.c (the logic),
 *  cradle.c (the TEACH ingest seam), galaxy.c (the CONSULT verb) and the
 *  host companion tools/mouthd/. It stays libc-light (plain C types +
 *  <stdint.h> only) so it drops into the LLM tier whose LLM_CFLAGS carry
 *  ONLY `-I <llm dir>` — no kernel headers, the same discipline student.h uses.
 *
 *  HOSTED-ONLY: frontier.c compiles into boot/linux + boot/linux_x86_64 +
 *  android (the LLM tier). Bare metal never references a frontier symbol
 *  (galaxy.c and cradle.c are hosted-only), so bare-metal .text is untouched
 *  and the crown stays byte-identical. frontier_stub.c carries the WEAK ABI
 *  for any FUTURE bare-metal reference (the student_stub.c pattern).
 */
#ifndef _FRONTIER_H_
#define _FRONTIER_H_

#include <stdint.h>

/* ── TEACH provenance: teacher_kind IS the license line (design §1.5) ───────
 * Enforced by the enum itself — API output can never become a lesson because
 * there is no value to name it. Only open-license LOCAL teachers may teach.  */
enum frontier_teacher_kind {
    FR_TEACHER_NONE            = 0,   /* unset / stripped header ⇒ refuse      */
    FR_TEACHER_SMOLLM2_LOCAL   = 1,   /* the committed SmolLM2 teacher fixture */
    FR_TEACHER_VOLUNTEER_LOCAL = 2    /* a volunteer's open-license local model*/
    /* NO FR_TEACHER_FRONTIER_API — the missing value is the anti-capture floor.*/
};

#define FR_MODEL_ID_MAX   48
#define FR_LICENSE_MAX    16

/* the lesson provenance header (design §5.3). Fixed layout, NO pointers, every
 * field ≤4-byte aligned so the content-id matches across ILP32/LP64. Precedes
 * the length-prefixed lesson body on the wire and in the p-fs lesson body.    */
typedef struct {
    uint8_t  teacher_kind;                 /* frontier_teacher_kind             */
    uint8_t  _pad[3];
    uint32_t origin_node;                  /* who ran the teacher (declaration) */
    char     model_id[FR_MODEL_ID_MAX];    /* NUL-padded model name             */
    char     license_tag[FR_LICENSE_MAX];  /* NUL-padded SPDX-ish tag (required)*/
} FR_TEACH_HDR;

/* ── kernel ↔ mouthd (localhost FM1 datagrams, design §5.1) ─────────────────
 * Versioned like relay v2, minus HMAC (mouthd binds 127.0.0.1 only). Layout:
 *   [0..2] magic "FM1"  [3] op (u8)  [4..7] req_id (u32 LE)  [8..] payload.   */
#define FM1_MAGIC0 'F'
#define FM1_MAGIC1 'M'
#define FM1_MAGIC2 '1'
#define FM1_HDR_LEN 8

enum frontier_fm1_op {
    FM1_OP_HELLO         = 1,  /* mouthd→kernel: mode + model_id + license once */
    FM1_OP_CONSULT_REQ   = 2,  /* kernel→mouthd: nonce + prompt                 */
    FM1_OP_CONSULT_CHUNK = 3,  /* mouthd→kernel: one reply chunk                */
    FM1_OP_CONSULT_DONE  = 4,  /* mouthd→kernel: status + nonce ECHO (§7.4)     */
    FM1_OP_CONSULT_ERR   = 5,  /* mouthd→kernel: error ⇒ honest degrade         */
    FM1_OP_TEACH_LESSON  = 6   /* mouthd→kernel: FR_TEACH_HDR + lesson body     */
};

/* mouthd mode byte (HELLO). */
enum frontier_mode {
    FR_MODE_CONSULT = 1,       /* --provider anthropic (TLS to the API)         */
    FR_MODE_TEACH   = 2        /* --teach http://127.0.0.1:PORT (open-license)  */
};

/* ── the anti-theater challenge transform (design §7.4). CONSULT_REQ carries a
 * per-request nonce; CONSULT_DONE must echo fr_echo_of(nonce). A stubbed
 * consult path that never reached the mouth cannot fake DONE — the kernel
 * rejects any reply whose echo mismatches and NEVER fabricates a citation.
 * mouthd includes this header and echoes the SAME transform. */
static inline uint32_t fr_echo_of(uint32_t nonce)
{
    return nonce * 2246822519u + 0x85EBCA77u;
}

/* ── the frame sink the CONSULT caller (galaxy.c) provides. frontier.c stages
 * each COMPLETE WS-JSON frame string; frontier.c OWNS the framing including the
 * load-bearing `src:"frontier"` label. (void*,frame,n) mirrors gx_chat_chunk.  */
typedef void (*fr_frame_fn)(void *ctx, const char *frame, int n);

/* ── CONSULT (called from galaxy.c, hosted-guarded). Prepends the R3-fact
 * context block, sends to mouthd (or the installed mock), holds the whole
 * reply, runs G-CHAT (CONS_SITE_CHAT_REPLY), then emits LABELED frames via
 * `frame`. Return:
 *     >0  consult completed (labeled frontier bytes emitted)
 *      0  NO MOUTH / not-consented / echo-mismatch: honest degrade — the caller
 *         prints the absence note and streams the student's OWN voice. NEVER a
 *         fabricated consult frame, model name or citation on this path.
 *     <0  conscience REFUSED (a plain refusal tok was emitted; the refusal IS
 *         the answer — the caller must NOT degrade over it).                    */
int frontier_consult(const char *text, int len, fr_frame_fn frame, void *ctx);

/* the exact production absence note (design §1.3). galaxy.c prints this to the
 * console on a 0 return; the cert greps for it. */
const char *frontier_degrade_note(void);

/* consent (design §1.4): the human's words leave the galaxy — first-use gate,
 * per node. galaxy.c calls grant() on the human's explicit ack. */
int  frontier_consent_ok(void);
void frontier_consent_grant(void);

/* status surface (design §1.4): "frontier (borrowed)" vs "own voice". */
const char *frontier_status_line(void);

/* ── TEACH ingest (the TEACH_LESSON handler + the [teach-prov] cert). Validates
 * the provenance header (teacher_kind ∈ {SMOLLM2_LOCAL,VOLUNTEER_LOCAL} AND a
 * non-empty license_tag — a stripped header is REFUSED, never silently
 * ingested), records the provenance, then rides cradle_lesson_ingest (whose
 * G-LEARN scan gates the bytes). Return >0 installed, 0 deferred, <0 refused.  */
int frontier_teach_ingest(const FR_TEACH_HDR *hdr, const uint8_t *body, int len);

/* last-accepted TEACH provenance (observability + the [teach-prov] cert). */
const char *frontier_last_teach_model(void);
const char *frontier_last_teach_license(void);
int         frontier_last_teach_kind(void);

/* the in-process cert suite ([frontier-*]) — hosted-only, MOCK mouthd (seeded
 * deterministic fixtures, no network, no key). Drives every load-bearing
 * falsifier and prints the gate lines CI greps. */
void frontier_self_test(void (*emit)(const char *));

#endif /* _FRONTIER_H_ */
