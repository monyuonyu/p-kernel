/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 2.0 Software Package
 *
 *    Copyright (C) 2006-2014 by Ken Sakamura.
 *    This software is distributed under the T-License 2.0.
 *----------------------------------------------------------------------
 *
 *    Released by T-Engine Forum(http://www.t-engine.org/) at 2014/09/01.
 *
 *----------------------------------------------------------------------
 */


#ifndef _MEMORY_H_
#define _MEMORY_H_

/*
 * Host-word-sized unsigned integer for pointer<->integer casts in
 * this allocator.
 *
 * The AreaQue scheme below packs the AREA_USE flag into the low bit
 * of QUEUE->prev. The original T-Kernel code cast QUEUE* through
 * `UW` (32-bit) to twiddle that bit, which silently truncated the
 * upper 32 bits of every pointer on any LP64 ABI (Linux x86-64,
 * Linux AArch64, AArch64 bare-metal at addresses >= 4 GiB). It is
 * currently latent on bare-metal builds because their heap addresses
 * happen to fit in 32 bits, but the truncation is a real bug and
 * fires the moment the heap moves above 4 GiB (e.g. the Linux port).
 *
 * `unsigned long` is exactly pointer-width on every ABI this
 * project targets (i386 ILP32, AArch64 / x86-64 LP64), so it is the
 * portable spelling without needing #ifdefs.
 */
typedef unsigned long	PTR_UINT;

/*
 * Memory allocation management information
 *
 *  Order of members must not be changed because members are used
 *  with casting from MPLCB.
 */
typedef struct {
	W		memsz;

	/* AreaQue for connecting each area where reserved pages are
	   divided Sort in ascending order of addresses in a page.
	   Do not sort between pages. */
	QUEUE		areaque;
	/* FreeQue for connecting unused area in reserved pages
	   Sort from small to large free spaces. */
	QUEUE		freeque;
} IMACB;

/*
 * Compensation for aligning "&areaque" position to 2 bytes border
 */
#define AlignIMACB(imacb)	( (IMACB*)((PTR_UINT)(imacb) & ~(PTR_UINT)0x1) )

/*
 * Minimum unit of subdivision
 *	The lower 1 bit of address is always 0
 *	because memory is allocated by ROUNDSZ.
 *	AreaQue uses the lower 1 bit for flag.
 */
#define ROUNDSZ		( sizeof(QUEUE) )	/* 8 bytes */
#define ROUND(sz)	( ((UW)(sz) + (UW)(ROUNDSZ-1)) & ~(UW)(ROUNDSZ-1) )

/* Minimum fragment size */
#define MIN_FRAGMENT	( sizeof(QUEUE) * 2 )

/*
 * Adjusting the size which can be allocated
 */
static inline W roundSize( W sz )
{
	if ( sz < (W)MIN_FRAGMENT ) {
		sz = (W)MIN_FRAGMENT;
	}
	return (W)(((UW)sz + (UW)(ROUNDSZ-1)) & ~(UW)(ROUNDSZ-1));
}


/*
 * Flag that uses the lower bits of AreaQue's 'prev'.
 *
 * These macros cast a QUEUE* through an integer to twiddle the low
 * bit. The integer must be at least pointer-width, otherwise the
 * upper bits of the pointer are silently dropped on LP64. Use
 * PTR_UINT (host-word-sized), not UW (32-bit).
 */
#define AREA_USE	0x00000001UL	/* In-use */
#define AREA_MASK	0x00000001UL

#define setAreaFlag(q, f)   ( (q)->prev = (QUEUE*)((PTR_UINT)(q)->prev |  (PTR_UINT)(f)) )
#define clrAreaFlag(q, f)   ( (q)->prev = (QUEUE*)((PTR_UINT)(q)->prev & ~(PTR_UINT)(f)) )
#define chkAreaFlag(q, f)   ( ((PTR_UINT)(q)->prev & (PTR_UINT)(f)) != 0 )

#define Mask(x)		( (QUEUE*)((PTR_UINT)(x) & ~(PTR_UINT)AREA_MASK) )
#define Assign(x, y)	( (x) = (QUEUE*)(((PTR_UINT)(x) & (PTR_UINT)AREA_MASK) | (PTR_UINT)(y)) )
/*
 * Area size
 *
 * FreeSize reads the area's byte size out of the 'prev' slot of the
 * second QUEUE following the free-area header (see knl_appendFreeArea
 * for the matching store). The size itself fits in W (32-bit), but it
 * was *written* as a QUEUE* (host-word-wide), so the read must go
 * through PTR_UINT first; casting straight to W on LP64 would invoke
 * the implementation-defined narrowing of a pointer to an int.
 */
#define AreaSize(aq)	( (VB*)(aq)->next - (VB*)((aq) + 1) )
#define FreeSize(fq)	( (W)(PTR_UINT)((fq) + 1)->prev )


IMPORT QUEUE* knl_searchFreeArea( IMACB *imacb, W blksz );
IMPORT void knl_appendFreeArea( IMACB *imacb, QUEUE *aq );
IMPORT void knl_removeFreeQue( QUEUE *fq );
IMPORT void knl_insertAreaQue( QUEUE *que, QUEUE *ent );
IMPORT void knl_removeAreaQue( QUEUE *aq );

IMPORT IMACB *knl_imacb;
IMPORT ER knl_init_Imalloc( void );

#endif /* _MEMORY_H_ */
