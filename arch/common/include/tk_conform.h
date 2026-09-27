/*
 *  tk_conform.h — the contract suite (inbox #5): checks the μT-Kernel 3.0
 *  tk_* promises p-kernel's user space relies on. See tk_conform.c.
 */
#ifndef TK_CONFORM_H
#define TK_CONFORM_H

/* Runs every check, printing "[tkc] PASS|FAIL ..." lines through `out`.
 * Returns the number of FAILs (0 = the contract holds on this kernel). */
INT tk_conform_run(void (*out)(const char *));

#endif
