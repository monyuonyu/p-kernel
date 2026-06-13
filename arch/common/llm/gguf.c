/*
 *  gguf.c — minimal libc-light GGUF reader (M1a). See gguf.h for scope.
 *
 *  Design notes:
 *   - mmap the whole file read-only; all string/array values are zero-copy
 *     views into the mapping (only the kv[] / tensors[] index arrays are
 *     malloc'd). 100MB+ files page in lazily, per inference-engine.md §2.
 *   - Every field read is bounds-checked against the file size via a single
 *     cursor (struct rdr) so a truncated / hostile file can never walk off
 *     the mapping. On any short read the parse fails with GGUF_E_TRUNC.
 *   - Little-endian only — GGUF defines no big-endian on-disk form. We read
 *     bytes explicitly (no unaligned native loads), so this is correct on
 *     any host regardless of its own endianness/alignment rules.
 *   - Hardcodes no model dimension: callers drive everything off the parsed
 *     metadata KVs and tensor table.
 */
#include "gguf.h"

#include <stdlib.h>      /* malloc/free                                   */
#include <string.h>      /* memcmp                                        */
#include <fcntl.h>       /* open                                          */
#include <unistd.h>      /* close, lseek                                  */
#include <sys/mman.h>    /* mmap                                          */
#include <sys/stat.h>    /* fstat                                         */

/* ---- error codes are declared in gguf.h (GGUF_OK / GGUF_E_*) ------------- */

const char *gguf_strerror(int err)
{
    switch (err) {
    case GGUF_OK:        return "ok";
    case GGUF_E_OPEN:    return "open/fstat/mmap failed";
    case GGUF_E_MAGIC:   return "bad magic (not a GGUF file)";
    case GGUF_E_VERSION: return "unsupported GGUF version";
    case GGUF_E_TRUNC:   return "truncated (read past end of file)";
    case GGUF_E_TYPE:    return "unknown metadata value type";
    case GGUF_E_OOM:     return "out of memory";
    case GGUF_E_DATAOFF: return "tensor data offset out of range";
    default:             return "unknown error";
    }
}

/* ---- a bounds-checked little-endian cursor ------------------------------ */
typedef struct {
    const uint8_t *p;      /* start of mapping                              */
    size_t         n;      /* total bytes                                   */
    size_t         off;    /* current cursor                                */
    int            err;    /* sticky error                                  */
} rdr;

static int rd_have(rdr *r, size_t k)
{
    if (r->err) return 0;
    if (k > r->n || r->off > r->n - k) { r->err = GGUF_E_TRUNC; return 0; }
    return 1;
}

static uint8_t rd_u8(rdr *r)
{
    if (!rd_have(r, 1)) return 0;
    return r->p[r->off++];
}

static uint16_t rd_u16(rdr *r)
{
    if (!rd_have(r, 2)) return 0;
    const uint8_t *q = r->p + r->off; r->off += 2;
    return (uint16_t)(q[0] | ((uint16_t)q[1] << 8));
}

static uint32_t rd_u32(rdr *r)
{
    if (!rd_have(r, 4)) return 0;
    const uint8_t *q = r->p + r->off; r->off += 4;
    return (uint32_t)q[0] | ((uint32_t)q[1] << 8) |
           ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
}

static uint64_t rd_u64(rdr *r)
{
    if (!rd_have(r, 8)) return 0;
    const uint8_t *q = r->p + r->off; r->off += 8;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)q[i] << (8 * i);
    return v;
}

static float rd_f32(rdr *r)
{
    uint32_t u = rd_u32(r);
    float f; memcpy(&f, &u, sizeof f);
    return f;
}

static double rd_f64(rdr *r)
{
    uint64_t u = rd_u64(r);
    double f; memcpy(&f, &u, sizeof f);
    return f;
}

/* GGUF string: uint64 len + raw bytes. Returns a zero-copy view. */
static gguf_str rd_str(rdr *r)
{
    gguf_str s = { NULL, 0 };
    uint64_t len = rd_u64(r);
    if (r->err) return s;
    if (len > r->n || r->off > r->n - len) { r->err = GGUF_E_TRUNC; return s; }
    s.ptr = (const char *)(r->p + r->off);
    s.len = len;
    r->off += len;
    return s;
}

/* ---- type-name tables --------------------------------------------------- */
const char *gguf_mtype_name(enum gguf_mtype t)
{
    switch (t) {
    case GGUF_T_UINT8:   return "u8";
    case GGUF_T_INT8:    return "i8";
    case GGUF_T_UINT16:  return "u16";
    case GGUF_T_INT16:   return "i16";
    case GGUF_T_UINT32:  return "u32";
    case GGUF_T_INT32:   return "i32";
    case GGUF_T_FLOAT32: return "f32";
    case GGUF_T_BOOL:    return "bool";
    case GGUF_T_STRING:  return "str";
    case GGUF_T_ARRAY:   return "arr";
    case GGUF_T_UINT64:  return "u64";
    case GGUF_T_INT64:   return "i64";
    case GGUF_T_FLOAT64: return "f64";
    default:             return "?";
    }
}

const char *ggml_type_name(enum ggml_type t)
{
    switch (t) {
    case GGML_TYPE_F32:  return "F32";
    case GGML_TYPE_F16:  return "F16";
    case GGML_TYPE_Q4_0: return "Q4_0";
    case GGML_TYPE_Q4_1: return "Q4_1";
    case GGML_TYPE_Q5_0: return "Q5_0";
    case GGML_TYPE_Q5_1: return "Q5_1";
    case GGML_TYPE_Q8_0: return "Q8_0";
    case GGML_TYPE_Q8_1: return "Q8_1";
    case GGML_TYPE_Q2_K: return "Q2_K";
    case GGML_TYPE_Q3_K: return "Q3_K";
    case GGML_TYPE_Q4_K: return "Q4_K";
    case GGML_TYPE_Q5_K: return "Q5_K";
    case GGML_TYPE_Q6_K: return "Q6_K";
    case GGML_TYPE_Q8_K: return "Q8_K";
    case GGML_TYPE_I8:   return "I8";
    case GGML_TYPE_I16:  return "I16";
    case GGML_TYPE_I32:  return "I32";
    case GGML_TYPE_I64:  return "I64";
    case GGML_TYPE_F64:  return "F64";
    case GGML_TYPE_BF16: return "BF16";
    default:             return "??";
    }
}

/* ggml block geometry: (block_size_in_elements, bytes_per_block). nbytes for
 * a tensor = n_elements / block_size * bytes_per_block. Used only to report a
 * size; M1a does not dequantize. 0,0 = unknown type (nbytes reported as 0). */
static void ggml_block_geom(enum ggml_type t, uint64_t *blk, uint64_t *bytes)
{
    switch (t) {
    case GGML_TYPE_F32:  *blk = 1;  *bytes = 4;   return;
    case GGML_TYPE_F16:  *blk = 1;  *bytes = 2;   return;
    case GGML_TYPE_BF16: *blk = 1;  *bytes = 2;   return;
    case GGML_TYPE_F64:  *blk = 1;  *bytes = 8;   return;
    case GGML_TYPE_I8:   *blk = 1;  *bytes = 1;   return;
    case GGML_TYPE_I16:  *blk = 1;  *bytes = 2;   return;
    case GGML_TYPE_I32:  *blk = 1;  *bytes = 4;   return;
    case GGML_TYPE_I64:  *blk = 1;  *bytes = 8;   return;
    /* block-quantized: 32-element super/sub blocks (ggml-quants.h) */
    case GGML_TYPE_Q4_0: *blk = 32; *bytes = 18;  return; /* 2 + 16          */
    case GGML_TYPE_Q4_1: *blk = 32; *bytes = 20;  return; /* 2 + 2 + 16      */
    case GGML_TYPE_Q5_0: *blk = 32; *bytes = 22;  return; /* 2 + 4 + 16      */
    case GGML_TYPE_Q5_1: *blk = 32; *bytes = 24;  return; /* 2 + 2 + 4 + 16  */
    case GGML_TYPE_Q8_0: *blk = 32; *bytes = 34;  return; /* 2 + 32          */
    case GGML_TYPE_Q8_1: *blk = 32; *bytes = 36;  return; /* 4 + 32          */
    /* k-quants: 256-element super-blocks */
    case GGML_TYPE_Q2_K: *blk = 256; *bytes = 84;  return;
    case GGML_TYPE_Q3_K: *blk = 256; *bytes = 110; return;
    case GGML_TYPE_Q4_K: *blk = 256; *bytes = 144; return;
    case GGML_TYPE_Q5_K: *blk = 256; *bytes = 176; return;
    case GGML_TYPE_Q6_K: *blk = 256; *bytes = 210; return;
    case GGML_TYPE_Q8_K: *blk = 256; *bytes = 292; return;
    default:             *blk = 0;   *bytes = 0;   return;
    }
}

/* ---- scalar size of a metadata value type (for skipping array elements) -- */
static size_t mtype_scalar_size(enum gguf_mtype t)
{
    switch (t) {
    case GGUF_T_UINT8: case GGUF_T_INT8: case GGUF_T_BOOL:    return 1;
    case GGUF_T_UINT16: case GGUF_T_INT16:                    return 2;
    case GGUF_T_UINT32: case GGUF_T_INT32: case GGUF_T_FLOAT32: return 4;
    case GGUF_T_UINT64: case GGUF_T_INT64: case GGUF_T_FLOAT64: return 8;
    default: return 0; /* STRING/ARRAY are variable-length, handled apart   */
    }
}

/* Decode one scalar value (non-ARRAY, non-STRING) into a kv. */
static void decode_scalar(rdr *r, enum gguf_mtype t, gguf_kv *kv)
{
    switch (t) {
    case GGUF_T_UINT8:   kv->val.u = rd_u8(r);  break;
    case GGUF_T_INT8:    kv->val.i = (int8_t)rd_u8(r);  break;
    case GGUF_T_UINT16:  kv->val.u = rd_u16(r); break;
    case GGUF_T_INT16:   kv->val.i = (int16_t)rd_u16(r); break;
    case GGUF_T_UINT32:  kv->val.u = rd_u32(r); break;
    case GGUF_T_INT32:   kv->val.i = (int32_t)rd_u32(r); break;
    case GGUF_T_FLOAT32: kv->val.f = (double)rd_f32(r); break;
    case GGUF_T_BOOL:    kv->val.u = rd_u8(r) ? 1u : 0u; break;
    case GGUF_T_UINT64:  kv->val.u = rd_u64(r); break;
    case GGUF_T_INT64:   kv->val.i = (int64_t)rd_u64(r); break;
    case GGUF_T_FLOAT64: kv->val.f = rd_f64(r); break;
    default:             r->err = GGUF_E_TYPE; break;
    }
}

/* Parse one metadata KV (key already... no: key + value here). */
static void parse_kv(rdr *r, gguf_kv *kv)
{
    memset(kv, 0, sizeof *kv);
    kv->key  = rd_str(r);
    kv->type = (enum gguf_mtype)rd_u32(r);
    if (r->err) return;

    if (kv->type == GGUF_T_STRING) {
        kv->str = rd_str(r);
        return;
    }
    if (kv->type == GGUF_T_ARRAY) {
        kv->arr_type = (enum gguf_mtype)rd_u32(r);
        kv->arr_len  = rd_u64(r);
        if (r->err) return;
        kv->arr_data = r->p + r->off;     /* view at first element          */
        /* skip the array body, validating bounds */
        if (kv->arr_type == GGUF_T_STRING) {
            for (uint64_t i = 0; i < kv->arr_len && !r->err; i++)
                (void)rd_str(r);
        } else if (kv->arr_type == GGUF_T_ARRAY) {
            r->err = GGUF_E_TYPE;          /* nested arrays not in GGUF spec  */
        } else {
            size_t sz = mtype_scalar_size(kv->arr_type);
            if (sz == 0) { r->err = GGUF_E_TYPE; return; }
            /* bounds: arr_len * sz must fit */
            if (kv->arr_len != 0 && sz > (r->n - r->off) / kv->arr_len) {
                r->err = GGUF_E_TRUNC; return;
            }
            r->off += (size_t)(kv->arr_len * sz);
        }
        return;
    }
    decode_scalar(r, kv->type, kv);
}

/* ---- public API --------------------------------------------------------- */
int gguf_open(gguf_file *gf, const char *path)
{
    memset(gf, 0, sizeof *gf);
    gf->fd = -1;
    gf->alignment = 32;     /* GGUF default per spec                         */

    gf->fd = open(path, O_RDONLY);
    if (gf->fd < 0) return GGUF_E_OPEN;

    struct stat st;
    if (fstat(gf->fd, &st) != 0 || st.st_size <= 0) {
        close(gf->fd); gf->fd = -1; return GGUF_E_OPEN;
    }
    gf->size = (size_t)st.st_size;

    void *m = mmap(NULL, gf->size, PROT_READ, MAP_PRIVATE, gf->fd, 0);
    if (m == MAP_FAILED) {
        close(gf->fd); gf->fd = -1; return GGUF_E_OPEN;
    }
    gf->base = (const uint8_t *)m;

    rdr r = { gf->base, gf->size, 0, 0 };

    /* header */
    uint8_t magic[4];
    magic[0] = rd_u8(&r); magic[1] = rd_u8(&r);
    magic[2] = rd_u8(&r); magic[3] = rd_u8(&r);
    if (r.err) { gguf_close(gf); return r.err; }
    if (memcmp(magic, "GGUF", 4) != 0) { gguf_close(gf); return GGUF_E_MAGIC; }

    gf->version   = rd_u32(&r);
    if (gf->version != 2 && gf->version != 3) {
        gguf_close(gf); return GGUF_E_VERSION;
    }
    gf->n_tensors = rd_u64(&r);
    gf->n_kv      = rd_u64(&r);
    if (r.err) { gguf_close(gf); return r.err; }

    /* sanity: counts can't exceed remaining bytes (each entry >= a few bytes) */
    if (gf->n_kv > gf->size || gf->n_tensors > gf->size) {
        gguf_close(gf); return GGUF_E_TRUNC;
    }

    /* metadata KVs */
    if (gf->n_kv) {
        gf->kv = (gguf_kv *)calloc(gf->n_kv, sizeof(gguf_kv));
        if (!gf->kv) { gguf_close(gf); return GGUF_E_OOM; }
    }
    for (uint64_t i = 0; i < gf->n_kv; i++) {
        parse_kv(&r, &gf->kv[i]);
        if (r.err) { gguf_close(gf); return r.err; }
    }

    /* general.alignment override (must come before computing data_offset) */
    {
        const gguf_kv *a = gguf_find(gf, "general.alignment");
        if (a && (a->type == GGUF_T_UINT32 || a->type == GGUF_T_UINT64 ||
                  a->type == GGUF_T_INT32  || a->type == GGUF_T_INT64)) {
            uint64_t al = (a->type == GGUF_T_INT32 || a->type == GGUF_T_INT64)
                          ? (uint64_t)a->val.i : a->val.u;
            if (al != 0) gf->alignment = al;
        }
    }

    /* tensor infos */
    if (gf->n_tensors) {
        gf->tensors = (gguf_tensor *)calloc(gf->n_tensors, sizeof(gguf_tensor));
        if (!gf->tensors) { gguf_close(gf); return GGUF_E_OOM; }
    }
    for (uint64_t i = 0; i < gf->n_tensors; i++) {
        gguf_tensor *t = &gf->tensors[i];
        t->name   = rd_str(&r);
        t->n_dims = rd_u32(&r);
        if (r.err) { gguf_close(gf); return r.err; }
        if (t->n_dims > GGUF_MAX_DIMS) { gguf_close(gf); return GGUF_E_TYPE; }
        for (uint32_t d = 0; d < t->n_dims; d++) t->dims[d] = rd_u64(&r);
        t->type   = (enum ggml_type)rd_u32(&r);
        t->offset = rd_u64(&r);
        if (r.err) { gguf_close(gf); return r.err; }
    }

    /* data section starts at the cursor, padded up to `alignment` */
    {
        uint64_t cur = (uint64_t)r.off;
        uint64_t pad = (gf->alignment - (cur % gf->alignment)) % gf->alignment;
        gf->data_offset = cur + pad;
    }
    if (gf->data_offset > gf->size) { gguf_close(gf); return GGUF_E_TRUNC; }

    /* resolve each tensor's absolute pointer + byte size, bounds-checked */
    for (uint64_t i = 0; i < gf->n_tensors; i++) {
        gguf_tensor *t = &gf->tensors[i];
        uint64_t nelem = (t->n_dims == 0) ? 0 : 1;
        for (uint32_t d = 0; d < t->n_dims; d++) nelem *= t->dims[d];

        uint64_t blk, bpb;
        ggml_block_geom(t->type, &blk, &bpb);
        if (blk == 0) {
            t->nbytes = 0;                 /* unknown type: don't guess       */
        } else {
            /* nelem should be a multiple of blk for quantized rows */
            uint64_t nblocks = (nelem + blk - 1) / blk;
            t->nbytes = nblocks * bpb;
        }

        uint64_t abs = gf->data_offset + t->offset;
        if (abs > gf->size ||
            (t->nbytes != 0 && (t->nbytes > gf->size || abs > gf->size - t->nbytes))) {
            gguf_close(gf);
            return GGUF_E_DATAOFF;
        }
        t->data = gf->base + abs;
    }

    return GGUF_OK;
}

void gguf_close(gguf_file *gf)
{
    if (!gf) return;
    if (gf->kv)      { free(gf->kv);      gf->kv = NULL; }
    if (gf->tensors) { free(gf->tensors); gf->tensors = NULL; }
    if (gf->base)    { munmap((void *)gf->base, gf->size); gf->base = NULL; }
    if (gf->fd >= 0) { close(gf->fd); gf->fd = -1; }
}

const gguf_kv *gguf_find(const gguf_file *gf, const char *key)
{
    size_t klen = strlen(key);
    for (uint64_t i = 0; i < gf->n_kv; i++) {
        const gguf_kv *kv = &gf->kv[i];
        if (kv->key.len == klen && memcmp(kv->key.ptr, key, klen) == 0)
            return kv;
    }
    return NULL;
}

int gguf_get_u64(const gguf_file *gf, const char *key, uint64_t *out)
{
    const gguf_kv *kv = gguf_find(gf, key);
    if (!kv) return 0;
    switch (kv->type) {
    case GGUF_T_UINT8: case GGUF_T_UINT16: case GGUF_T_UINT32:
    case GGUF_T_UINT64: case GGUF_T_BOOL:
        *out = kv->val.u; return 1;
    case GGUF_T_INT8: case GGUF_T_INT16: case GGUF_T_INT32: case GGUF_T_INT64:
        if (kv->val.i < 0) return 0;
        *out = (uint64_t)kv->val.i; return 1;
    default:
        return 0;
    }
}

int gguf_get_f32(const gguf_file *gf, const char *key, float *out)
{
    const gguf_kv *kv = gguf_find(gf, key);
    if (!kv) return 0;
    if (kv->type == GGUF_T_FLOAT32 || kv->type == GGUF_T_FLOAT64) {
        *out = (float)kv->val.f; return 1;
    }
    return 0;
}

int gguf_get_str(const gguf_file *gf, const char *key, gguf_str *out)
{
    const gguf_kv *kv = gguf_find(gf, key);
    if (!kv || kv->type != GGUF_T_STRING) return 0;
    *out = kv->str; return 1;
}
