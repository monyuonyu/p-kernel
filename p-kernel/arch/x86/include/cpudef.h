/*
 *  cpudef.h (x86)
 *  x86 CPU register structures for T-Kernel tk_get_reg/tk_set_reg
 */

#ifndef __TK_CPUDEF_H__
#define __TK_CPUDEF_H__

#ifdef __cplusplus
extern "C" {
#endif

/*
 * General purpose registers (x86 callee-saved)
 */
typedef struct t_regs {
    VW  ebx;
    VW  esi;
    VW  edi;
    VW  ebp;
} T_REGS;

/*
 * Exception-related registers
 */
typedef struct t_eit {
    void    *pc;        /* EIP - program counter */
    UW      eflags;     /* EFLAGS */
    UW      taskmode;   /* Task mode flag */
} T_EIT;

/*
 * Control registers
 */
typedef struct t_cregs {
    void    *ssp;       /* System stack pointer (ESP) */
} T_CREGS;

#ifdef __cplusplus
}
#endif

#endif /* __TK_CPUDEF_H__ */
