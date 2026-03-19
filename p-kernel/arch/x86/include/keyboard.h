/*
 *  keyboard.h (x86)
 *  PS/2 keyboard driver via IRQ1
 */

#pragma once
#include "kernel.h"

/*
 * kbd_init - initialize keyboard driver
 *   sem: T-Kernel semaphore ID used to wake shell on keypress
 */
void kbd_init(ID sem);

/*
 * kbd_getchar - blocking read (waits on semaphore)
 */
char kbd_getchar(void);

/*
 * kbd_available - non-blocking check: returns 1 if key is ready
 */
INT kbd_available(void);
