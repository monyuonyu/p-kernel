/*
 *  tensor.c (x86)
 *  Kernel-managed tensor memory
 *
 *  Uses a static pool of descriptors and a single shared bump-allocator
 *  data region.  Bump pointer rewinds when the last-allocated block is
 *  freed (LIFO pattern matches inference pipeline usage).
 *
 *  All data is 32-byte aligned — ready for future SIMD / DMA use.
 */

#include "kernel.h"
#include "ai_kernel.h"
#include <tmonitor.h>

/* ------------------------------------------------------------------ */
/* Static pools                                                        */
/* ------------------------------------------------------------------ */

static TENSOR_DESC desc_pool[TENSOR_POOL_SIZE];

/* 32-byte aligned data pool */
static UB data_pool[TENSOR_DATA_POOL_KB * 1024] __attribute__((aligned(TENSOR_ALIGN)));
static UW data_top = 0;    /* next free offset in data_pool */

static ID pool_sem = 0;    /* binary semaphore (mutex) */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static UW dtype_elem_size(UB dtype)
{
    if (dtype == TENSOR_DTYPE_I8)  return 1;
    if (dtype == TENSOR_DTYPE_I32) return 4;
    return 4;   /* TENSOR_DTYPE_F32 */
}

static UW calc_nbytes(UB ndim, const UW shape[], UB dtype)
{
    UW n = dtype_elem_size(dtype);
    for (UB i = 0; i < ndim && i < TENSOR_MAX_DIM; i++) n *= shape[i];
    return n;
}

/* Round up to TENSOR_ALIGN */
static UW align_up(UW v)
{
    return (v + TENSOR_ALIGN - 1) & ~((UW)(TENSOR_ALIGN - 1));
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                      */
/* ------------------------------------------------------------------ */

void tensor_init(void)
{
    T_CSEM cs = { .exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 1, .maxsem = 1 };
    pool_sem = tk_cre_sem(&cs);
    for (INT i = 0; i < TENSOR_POOL_SIZE; i++) desc_pool[i].in_use = 0;
    data_top = 0;
}

/* ------------------------------------------------------------------ */
/* Create                                                              */
/* ------------------------------------------------------------------ */

ID tk_cre_tensor(UB ndim, const UW shape[], UB dtype, UB layout)
{
    if (ndim == 0 || ndim > TENSOR_MAX_DIM) return (ID)E_PAR;
    if (pool_sem <= 0) return (ID)E_NOEXS;

    UW nbytes  = calc_nbytes(ndim, shape, dtype);
    UW aligned = align_up(nbytes);

    tk_wai_sem(pool_sem, 1, TMO_FEVR);

    /* Find free descriptor slot */
    INT slot = -1;
    for (INT i = 0; i < TENSOR_POOL_SIZE; i++) {
        if (!desc_pool[i].in_use) { slot = i; break; }
    }
    if (slot < 0) { tk_sig_sem(pool_sem, 1); return (ID)E_LIMIT; }

    /* Allocate from bump pool */
    if (data_top + aligned > (UW)(TENSOR_DATA_POOL_KB * 1024)) {
        tk_sig_sem(pool_sem, 1); return (ID)E_NOMEM;
    }

    TENSOR_DESC *d = &desc_pool[slot];
    d->dtype  = dtype;
    d->layout = layout;
    d->ndim   = ndim;
    d->nbytes = nbytes;
    d->data   = data_pool + data_top;
    d->in_use = 1;

    for (UB i = 0; i < TENSOR_MAX_DIM; i++)
        d->shape[i] = (i < ndim) ? shape[i] : 1;

    data_top += aligned;

    tk_sig_sem(pool_sem, 1);
    return (ID)slot;
}

/* ------------------------------------------------------------------ */
/* Delete                                                              */
/* ------------------------------------------------------------------ */

ER tk_del_tensor(ID tid)
{
    if (tid < 0 || tid >= (ID)TENSOR_POOL_SIZE) return E_ID;

    tk_wai_sem(pool_sem, 1, TMO_FEVR);

    TENSOR_DESC *d = &desc_pool[tid];
    if (!d->in_use) { tk_sig_sem(pool_sem, 1); return E_NOEXS; }

    /* Reclaim data space if this was the last bump allocation */
    UW offset  = (UW)((UB *)d->data - data_pool);
    UW aligned = align_up(d->nbytes);
    if (offset + aligned == data_top)
        data_top = offset;

    d->in_use = 0;
    tk_sig_sem(pool_sem, 1);
    return E_OK;
}

/* ------------------------------------------------------------------ */
/* Zero-copy accessors                                                 */
/* ------------------------------------------------------------------ */

void *tk_tensor_ptr(ID tid)
{
    if (tid < 0 || tid >= (ID)TENSOR_POOL_SIZE || !desc_pool[tid].in_use)
        return NULL;
    return desc_pool[tid].data;
}

UW tk_tensor_size(ID tid)
{
    if (tid < 0 || tid >= (ID)TENSOR_POOL_SIZE || !desc_pool[tid].in_use)
        return 0;
    return desc_pool[tid].nbytes;
}

ER tk_tensor_write(ID tid, UW off, const void *src, UW len)
{
    if (tid < 0 || tid >= (ID)TENSOR_POOL_SIZE || !desc_pool[tid].in_use)
        return E_ID;
    if (off + len > desc_pool[tid].nbytes) return E_PAR;

    UB       *d = (UB *)desc_pool[tid].data + off;
    const UB *s = (const UB *)src;
    for (UW i = 0; i < len; i++) d[i] = s[i];
    return E_OK;
}

ER tk_tensor_read(ID tid, UW off, void *dst, UW len)
{
    if (tid < 0 || tid >= (ID)TENSOR_POOL_SIZE || !desc_pool[tid].in_use)
        return E_ID;
    if (off + len > desc_pool[tid].nbytes) return E_PAR;

    UB       *d = (UB *)dst;
    const UB *s = (const UB *)desc_pool[tid].data + off;
    for (UW i = 0; i < len; i++) d[i] = s[i];
    return E_OK;
}

ER tk_tensor_zero(ID tid)
{
    if (tid < 0 || tid >= (ID)TENSOR_POOL_SIZE || !desc_pool[tid].in_use)
        return E_ID;
    UB *d = (UB *)desc_pool[tid].data;
    for (UW i = 0; i < desc_pool[tid].nbytes; i++) d[i] = 0;
    return E_OK;
}
