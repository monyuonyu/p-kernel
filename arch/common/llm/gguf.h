/*
 *  gguf.h — minimal libc-light GGUF (llama.cpp weight format) reader.
 *
 *  Milestone M1a (docs/architecture/inference-engine.md §8.1):
 *  read a GGUF file's header + ALL metadata KV pairs + the tensor table,
 *  and expose them so a caller can print the model config and enumerate the
 *  weight tensors. No dequant, no matmul, no forward — those are M1b/M1c.
 *
 *  Scope / honesty (conversation.md §2): this is a HOST / Android-side loader,
 *  NOT bare-metal kernel code. The "身体" (the device that holds the weights)
 *  is host/Android, so this TU is allowed to use mmap/open/read + <stdint.h>.
 *  It is deliberately NOT part of the bare-metal arch/common include chain and
 *  is compiled only by the host test harness (and, later, the host inference
 *  engine). It is libc-light: it uses mmap + a handful of byte helpers, parses
 *  everything from the file's own metadata, and hardcodes no model dimension.
 *
 *  GGUF v2/v3 layout (little-endian, the only endianness GGUF defines):
 *    char     magic[4]            "GGUF"
 *    uint32   version             (2 or 3 supported here)
 *    uint64   tensor_count
 *    uint64   metadata_kv_count
 *    metadata_kv[metadata_kv_count]:
 *        gguf_string  key         (uint64 len + bytes)
 *        uint32       value_type  (enum gguf_mtype)
 *        value                    (typed; ARRAY carries its own elem type+len)
 *    tensor_info[tensor_count]:
 *        gguf_string  name
 *        uint32       n_dims
 *        uint64       dims[n_dims]
 *        uint32       ggml_type
 *        uint64       offset       (relative to the start of the data section)
 *    <pad to general.alignment>   (default 32)
 *    tensor data ...
 */
#ifndef PKERNEL_GGUF_H
#define PKERNEL_GGUF_H

/* M1a carries its own version (modver registry; compatibility.md): the
 * GGUF reader's contract version. v1 = GGUF v2/v3 header + metadata KV +
 * tensor-table reader (no dequant; that is GGUF data parsing only). */
#define GGUF_VER  1

#include <stdint.h>
#include <stddef.h>

/* GGUF metadata value types (the on-disk uint32 tag). */
enum gguf_mtype {
    GGUF_T_UINT8   = 0,
    GGUF_T_INT8    = 1,
    GGUF_T_UINT16  = 2,
    GGUF_T_INT16   = 3,
    GGUF_T_UINT32  = 4,
    GGUF_T_INT32   = 5,
    GGUF_T_FLOAT32 = 6,
    GGUF_T_BOOL    = 7,
    GGUF_T_STRING  = 8,
    GGUF_T_ARRAY   = 9,
    GGUF_T_UINT64  = 10,
    GGUF_T_INT64   = 11,
    GGUF_T_FLOAT64 = 12,
    GGUF_MTYPE_COUNT
};

/* ggml tensor element types — the subset we need to *name* (M1a never
 * dequantizes). Values are the canonical ggml type ids. */
enum ggml_type {
    GGML_TYPE_F32    = 0,
    GGML_TYPE_F16    = 1,
    GGML_TYPE_Q4_0   = 2,
    GGML_TYPE_Q4_1   = 3,
    GGML_TYPE_Q5_0   = 6,
    GGML_TYPE_Q5_1   = 7,
    GGML_TYPE_Q8_0   = 8,
    GGML_TYPE_Q8_1   = 9,
    GGML_TYPE_Q2_K   = 10,
    GGML_TYPE_Q3_K   = 11,
    GGML_TYPE_Q4_K   = 12,
    GGML_TYPE_Q5_K   = 13,
    GGML_TYPE_Q6_K   = 14,
    GGML_TYPE_Q8_K   = 15,
    GGML_TYPE_I8     = 16,
    GGML_TYPE_I16    = 17,
    GGML_TYPE_I32    = 18,
    GGML_TYPE_I64    = 19,
    GGML_TYPE_F64    = 20,
    GGML_TYPE_BF16   = 30,
    GGML_TYPE_COUNT_HINT = 64
};

#define GGUF_MAX_DIMS 4

/* A non-owning view of a string inside the mmap'd file: not NUL-terminated. */
typedef struct {
    const char *ptr;
    uint64_t    len;
} gguf_str;

/* One metadata KV. For scalars the value is decoded into the union. For
 * STRING, `str` views into the file. For ARRAY, only the element type, count
 * and a pointer to the first element are kept (M1a does not need to expand
 * arrays such as the tokenizer vocab — it only enumerates/prints summaries). */
typedef struct {
    gguf_str        key;
    enum gguf_mtype type;          /* the KV's declared value type           */
    /* scalar payloads (valid per `type`) */
    union {
        uint64_t u;                /* all unsigned ints, BOOL                */
        int64_t  i;                /* all signed ints                        */
        double   f;                /* FLOAT32 widened, FLOAT64               */
    } val;
    gguf_str        str;           /* valid iff type == GGUF_T_STRING        */
    /* array payloads (valid iff type == GGUF_T_ARRAY) */
    enum gguf_mtype arr_type;      /* element type                           */
    uint64_t        arr_len;       /* element count                          */
    const uint8_t  *arr_data;      /* first element, raw in-file bytes       */
} gguf_kv;

/* One tensor-table entry. `data` points at the tensor's bytes in the mmap. */
typedef struct {
    gguf_str        name;
    uint32_t        n_dims;
    uint64_t        dims[GGUF_MAX_DIMS];
    enum ggml_type  type;
    uint64_t        offset;        /* relative to data section start         */
    const uint8_t  *data;          /* absolute pointer into the mmap         */
    uint64_t        nbytes;        /* size in bytes (computed; 0 if unknown) */
} gguf_tensor;

typedef struct {
    /* mmap bookkeeping */
    int             fd;
    const uint8_t  *base;          /* mmap base                              */
    size_t          size;          /* file size                              */

    uint32_t        version;
    uint64_t        n_tensors;
    uint64_t        n_kv;

    gguf_kv        *kv;            /* malloc'd, n_kv entries                  */
    gguf_tensor    *tensors;       /* malloc'd, n_tensors entries            */

    uint64_t        alignment;     /* general.alignment (default 32)         */
    uint64_t        data_offset;   /* file offset of the tensor data section */
} gguf_file;

/* Return codes (gguf_open). 0 = ok; negatives map to gguf_strerror(). */
#define GGUF_OK          0
#define GGUF_E_OPEN     (-1)   /* open()/fstat()/mmap() failed              */
#define GGUF_E_MAGIC    (-2)   /* not a GGUF file                           */
#define GGUF_E_VERSION  (-3)   /* unsupported GGUF version                  */
#define GGUF_E_TRUNC    (-4)   /* read ran past end of file                 */
#define GGUF_E_TYPE     (-5)   /* unknown metadata value type               */
#define GGUF_E_OOM      (-6)   /* malloc failed                             */
#define GGUF_E_DATAOFF  (-7)   /* a tensor's data lies outside the file     */

/* Open + parse. Returns 0 on success, negative on error (see gguf_strerror).
 * On success the caller must gguf_close(). */
int  gguf_open(gguf_file *gf, const char *path);
void gguf_close(gguf_file *gf);

const char *gguf_strerror(int err);

/* Metadata lookup by NUL-terminated key. Returns NULL if absent. */
const gguf_kv *gguf_find(const gguf_file *gf, const char *key);

/* Typed convenience getters. Each returns 1 + writes *out on success (with
 * sane numeric coercion across the int width zoo), else 0 (out untouched). */
int gguf_get_u64(const gguf_file *gf, const char *key, uint64_t *out);
int gguf_get_f32(const gguf_file *gf, const char *key, float    *out);
int gguf_get_str(const gguf_file *gf, const char *key, gguf_str *out);

/* Human-readable type names (static storage, never NULL). */
const char *gguf_mtype_name(enum gguf_mtype t);
const char *ggml_type_name(enum ggml_type t);

#endif /* PKERNEL_GGUF_H */
