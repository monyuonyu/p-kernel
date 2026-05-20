/*
 *  cpudef.h (aarch64)
 *  AArch64 CPU register structures for T-Kernel tk_get_reg/tk_set_reg
 */

#ifndef __TK_CPUDEF_H__
#define __TK_CPUDEF_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Callee-saved general purpose registers */
typedef struct t_regs {
    VW  x19;
    VW  x20;
    VW  x21;
    VW  x22;
    VW  x23;
    VW  x24;
    VW  x25;
    VW  x26;
    VW  x27;
    VW  x28;
    VW  x29;    /* frame pointer */
} T_REGS;

/* Exception-related registers */
typedef struct t_eit {
    void    *pc;        /* ELR_EL1 — faulting/return PC */
    UW      spsr;       /* SPSR_EL1 */
    UW      taskmode;
} T_EIT;

/* Control registers */
typedef struct t_cregs {
    void    *ssp;       /* System stack pointer */
} T_CREGS;

#ifdef __cplusplus
}
#endif

#endif /* __TK_CPUDEF_H__ */
