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

/*
 *	@(#)typedef.h
 *
 *	Standard data type definitions
 */

#ifndef	__TYPEDEF_H__
#define __TYPEDEF_H__

#ifdef __cplusplus
extern "C" {
#endif

#ifdef TKERNEL_CHECK_CONST
#define CONST	const
#else
#define CONST
#endif

/*
 * General-purpose data type  
 */
typedef signed char	    S1; /* Signed 8 bit integer    */
typedef signed short    S2; /* Signed 16 bit integer   */
typedef signed long	    S4; /* Signed 32 bit integer   */
typedef unsigned char   U1; /* Unsigned 8 bit integer  */
typedef unsigned short  U2; /* Unsigned 16 bit integer */
typedef unsigned long   U4; /* Unsigned 32 bit integer */

typedef signed char        int8_t;   /* Signed 8 bit integer    */
typedef signed short        int16_t;  /* Signed 16 bit integer   */
typedef signed long        int32_t;  /* Signed 32 bit integer   */
typedef signed long long   int64_t;  /* Signed 64 bit integer   */
typedef unsigned char      uint8_t;  /* Unsigned 8 bit integer  */
typedef unsigned short     uint16_t; /* Unsigned 16 bit integer */
typedef unsigned long      uint32_t; /* Unsigned 32 bit integer */
typedef unsigned long long uint64_t; /* Unsigned 64 bit integer */

typedef signed char	B;	/* Signed 8 bit integer */
typedef signed short	H;	/* Signed 16 bit integer */
typedef signed long	W;	/* Signed 32 bit integer */
typedef unsigned char	UB;	/* Unsigned 8 bit integer */
typedef unsigned short  UH;	/* Unsigned 16 bit integer */
typedef unsigned long	UW;	/* Unsigned 32 bit integer */

typedef signed char	VB;	/* Nonuniform type 8 bit data */
typedef signed short	VH;	/* Nonuniform type 16 bit data */
typedef signed long	VW;	/* Nonuniform type 32 bit data */
typedef void		*VP;	/* Nonuniform type data pointer */

typedef volatile B	_B;	/* Volatile statement attached */
typedef volatile H	_H;
typedef volatile W	_W;
typedef volatile UB	_UB;
typedef volatile UH	_UH;
typedef volatile UW	_UW;

typedef signed int	INT;	/* Processor bit width signed integer */
typedef unsigned int	UINT;	/* Processor bit width unsigned integer */

typedef W		SZ;	/* Size general */

typedef INT		ID;	/* ID general */
typedef	W		MSEC;	/* Time general (millisecond) */

typedef void		(*FP)();	/* Function address general */
typedef INT		(*FUNCP)();	/* Function address general */

#define LOCAL		static		/* Local symbol definition */
#define EXPORT				/* Global symbol definition */
#define IMPORT		extern		/* Global symbol reference */

/*
 * Boolean value 
 *	Defined as TRUE = 1, but it is always true when not 0.
 *	Thus, comparison such as bool = TRUE are not permitted.
 *	Should be as per bool !=FALSE.
 */
typedef UINT		BOOL;
#define TRUE		1	/* True */
#define FALSE		0	/* False */

/*
 * TRON character code
 */
typedef UH		TC;		/* TRON character code */
#define TNULL		((TC)0)		/* End of TRON code character string */

#ifdef __cplusplus
}
#endif
#endif /* __TYPEDEF_H__ */
