/*
 *  cpu_conf.h (x86)
 *  CPU-Dependent OS Configuration
 */

#ifndef _CPU_CONF_
#define _CPU_CONF_

/*
 * Minimum system stack size per task.
 * Must be larger than SStackFrame.
 * x86: 6 saved regs (24B) + return addr + task args = 48B minimum
 */
#define MIN_SYS_STACK_SIZE  512

#endif /* _CPU_CONF_ */
