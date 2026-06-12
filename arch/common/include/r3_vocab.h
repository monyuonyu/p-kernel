/* ------------------------------------------------------------------ *
 *  r3_vocab.h — LM-8 (living-mind.md Part IX): the fixed, embedded,
 *  content-addressed word list. Real WORDS in, one real WORD out.
 *
 *  WORD LIST v2 (LM-9 Part X): ENGLISH, 16 key words + 64 answer words —
 *  grown in lock-step with the R_DM 32->48 surgery (the 8/32 words are kept
 *  as a prefix so the /vocab content-id change is honest). Multilingual is a
 *  capacity follow-up; the manifesto
 *  stays 32-language (ark-profile unchanged) — only the toy *vocabulary*
 *  is English v1. A non-English typer gets an HONEST refusal, never a
 *  mojibake binding (IX.3).
 *
 *  Mechanism (IX.3): two embedded UTF-8 byte images (newline-separated
 *  words). Token id = the word's 0-based line index. Each image has a
 *  content-id (pfs_id_compute — THE one hash), served to the UI via
 *  GET /vocab so the page and the kernel PROVABLY share the same list:
 *  a mismatch is a detectable id disagreement, NOT a silent fake binding.
 *
 *  OOV (IX.0 #6) is an HONEST REFUSAL: a word not on the list has no
 *  token id; r3_vocab_id returns -1 and the mouth PRINTS the refusal.
 *  Silently hashing OOV words into collisions would manufacture fake
 *  bindings — the opposite of the project's honesty discipline.
 * ------------------------------------------------------------------ */
#ifndef R3_VOCAB_H
#define R3_VOCAB_H

#include <typedef.h>
#include "pfs_block.h"     /* PFS_ID_LEN, pfs_id_compute */

/* the two vocabularies' sizes — MUST equal R_KEYV / R_VALV in
 * r3_incontext.c (a build-time _Static_assert there pins them). */
/* LM-9 (living-mind Part X) dims: the R_DM 32->48 surgery WIDENED the thinking
 * width, so the vocab grows in lock-step with the substrate (R_KEYV 8->16,
 * R_VALV 32->64 in r3_incontext.c). The 8/32 words stay as a PREFIX so the
 * /vocab content-id change is honest (token ids 0..7 / 0..31 still resolve to
 * the SAME words; ids 8..15 / 32..63 are the new ones). */
#define R3_VOCAB_KEYS   16      /* key words: the things a person asks ABOUT */
#define R3_VOCAB_VALS   64      /* answer words: the one-word answers        */

/* word -> token id (0-based line index), or -1 if not in the list (OOV).
 * `len` may be 0 to NUL-terminate; otherwise the first `len` bytes are
 * matched exactly (case-sensitive, no trimming — the UI sends clean words). */
INT r3_vocab_key_id(const char *w, UW len);   /* into the KEY vocabulary  */
INT r3_vocab_val_id(const char *w, UW len);   /* into the ANSWER vocab    */

/* token id -> word (NUL-terminated, points into the embedded image), or
 * NULL if the id is out of range. */
const char *r3_vocab_key_word(INT id);
const char *r3_vocab_val_word(INT id);

/* list sizes (always R3_VOCAB_KEYS / R3_VOCAB_VALS; the function form lets
 * the cert/UI read them without the macro). */
UW r3_vocab_key_count(void);
UW r3_vocab_val_count(void);

/* content-ids of the EXACT embedded byte images (pfs_id_compute). The UI
 * fetches these via GET /vocab; the cert asserts the page and the kernel
 * agree by content-id (kernel and page can never disagree silently). */
void r3_vocab_key_id_blob(U1 out[PFS_ID_LEN]);
void r3_vocab_val_id_blob(U1 out[PFS_ID_LEN]);

/* raw image bytes (newline-separated, NUL-terminated) — GET /vocab streams
 * these so a typer sees exactly the words that exist (a datalist). */
const char *r3_vocab_key_blob(UW *len_out);
const char *r3_vocab_val_blob(UW *len_out);

#endif /* R3_VOCAB_H */
