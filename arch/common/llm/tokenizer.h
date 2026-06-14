/*
 *  tokenizer.h — SmolLM2-135M BPE tokenizer (GPT-2-family byte-level BPE).
 *
 *  Milestone M1d (docs/architecture/inference-engine.md §4):
 *  the BPE tokenizer the M1c forward (forward.c) needs to consume RAW TEXT.
 *  M1c's lm_generate() takes PRE-TOKENIZED ids; this turns text<->ids so the
 *  engine ("the teacher") can be driven by a string.
 *
 *  Everything is driven from the GGUF metadata loaded by M1a (gguf.c):
 *    tokenizer.ggml.tokens      — the vocab (string array, byte-level encoded)
 *    tokenizer.ggml.merges      — the BPE merge rules ("first second" per entry,
 *                                 rank == array index; lower rank merges first)
 *    tokenizer.ggml.token_type  — per-token type (NORMAL/CONTROL/BYTE/...)
 *    tokenizer.ggml.{bos,eos,unknown,padding}_token_id — special ids if present
 *  NOTHING about SmolLM2 is hardcoded except the GPT-2 byte<->unicode map and
 *  the GPT-2/smollm pre-tokenizer split, which are the *family* contract; a
 *  different gpt2-family GGUF (same pre-tokenizer) loads and runs unchanged.
 *
 *  Scope / honesty (conversation.md §2): HOST / Android-side ("身体") code, the
 *  same tier as gguf.c / quant.c / forward.c. It is libc-light: <stdint.h>/
 *  <stddef.h> + malloc/free only (one set of allocs at tok_load, freed at
 *  tok_free; encode/decode use caller-provided buffers, no per-call malloc).
 *  No transcendental math, so the wave-49 "one math everywhere" rule is moot
 *  here; built with the same -O1 -ffp-contract=off as the rest of llm/.
 *
 *  Matches llama.cpp's llama_tokenize() for this model: GPT-2 byte-level BPE,
 *  the "smollm" pre-tokenizer (digits split singly, then the GPT-2 word split),
 *  add_space_prefix=false, add_bos_token=false. See tokenizer_test.c for the
 *  oracle cert. Special-token text (e.g. "<|im_start|>") is NOT parsed out of
 *  raw text here (parse_special=false), matching the M1c oracle's call.
 */
#ifndef PKERNEL_LLM_TOKENIZER_H
#define PKERNEL_LLM_TOKENIZER_H

/* M1d carries its own version (modver registry; compatibility.md): the
 * GPT-2-family byte-level BPE tokenizer contract. v1 = encode/decode driven
 * entirely from GGUF metadata (vocab + merges). */
#define LLM_TOKENIZER_VER  1

#include <stdint.h>
#include <stddef.h>
#include "gguf.h"

/* token type ids (the ggml tokenizer.ggml.token_type values). */
enum tok_type {
    TOK_TYPE_UNDEFINED   = 0,
    TOK_TYPE_NORMAL      = 1,
    TOK_TYPE_UNKNOWN     = 2,
    TOK_TYPE_CONTROL     = 3,   /* special / control tokens (e.g. <|im_end|>)  */
    TOK_TYPE_USER_DEFINED= 4,
    TOK_TYPE_UNUSED      = 5,
    TOK_TYPE_BYTE        = 6     /* single-byte fallback tokens                 */
};

/* one vocab entry: a (non-owning) view into the mmap'd GGUF string. */
typedef struct {
    const char *str;     /* byte-level-encoded token text (not NUL-terminated) */
    uint32_t    len;
    int32_t     type;    /* enum tok_type                                       */
} tok_vocab_entry;

typedef struct {
    const gguf_file *gf;          /* not owned; must outlive the tokenizer      */

    /* vocab */
    tok_vocab_entry *vocab;       /* malloc'd, n_vocab entries                   */
    int              n_vocab;

    /* token-string -> id hash map (open addressing over the vocab strings)     */
    int32_t         *htab;        /* malloc'd, hcap entries; -1 = empty          */
    int              hcap;        /* power of two                                */

    /* merge ranks: a parallel hash map keyed by (left_id<<32 | right_id) is not
     * possible before ids exist, so we key by the concatenated UTF-8 strings.
     * Stored as a hash map from "left\0right" -> rank.                          */
    struct tok_merge *merges;     /* malloc'd, n_merges entries                  */
    int               n_merges;
    int32_t          *mtab;       /* malloc'd, mcap entries; -1 = empty          */
    int               mcap;       /* power of two                                */

    /* special ids (-1 if absent in metadata) */
    int32_t bos_id, eos_id, unk_id, pad_id;

    /* byte -> token-id for the 256 single-byte fallbacks (-1 if unmapped)      */
    int32_t byte_tok[256];
} tokenizer;

/* return codes */
#define TOK_OK         0
#define TOK_E_META   (-1)   /* a required tokenizer metadata key is missing     */
#define TOK_E_OOM    (-2)   /* malloc failed                                    */
#define TOK_E_TYPE   (-3)   /* a metadata array had an unexpected element type  */
#define TOK_E_BUF    (-4)   /* a caller output buffer was too small             */

const char *tok_strerror(int e);

/* Build the tokenizer from an already-opened GGUF. gf must outlive `t`
 * (we hold views into its mmap). Returns 0 / negative. Call tok_free() after. */
int  tok_load(tokenizer *t, const gguf_file *gf);
void tok_free(tokenizer *t);

/* encode(text) -> token ids. `text` is `n` raw bytes (UTF-8). Writes up to
 * `cap` ids into out[]; returns the number written, or TOK_E_BUF if cap is too
 * small. add_bos: prepend bos_id if true and bos_id>=0 (llama add_special).   */
int  tok_encode(const tokenizer *t, const char *text, size_t n,
                int add_bos, int32_t *out, int cap);

/* decode(ids) -> text. Writes raw UTF-8 bytes into out[] (NOT NUL-terminated;
 * the caller may NUL-terminate using the returned length). Returns the number
 * of bytes written, or TOK_E_BUF if cap is too small. */
int  tok_decode(const tokenizer *t, const int32_t *ids, int n,
                char *out, size_t cap);

/* id <-> the byte-level-encoded token string (for tests/inspection). */
int  tok_id_of(const tokenizer *t, const char *str, size_t len); /* -1 if none */

#endif /* PKERNEL_LLM_TOKENIZER_H */
