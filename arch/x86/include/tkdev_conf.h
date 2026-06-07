/*
 *  tkdev_conf.h (x86)
 *  x86/QEMU hardware configuration
 */

#ifndef _TKDEV_CONF_
#define _TKDEV_CONF_

/* PIT 8253 base frequency (Hz) */
#define PIT_BASE_HZ     1193182UL

/* Timer interrupt frequency (Hz) - 100Hz = 10ms period */
#define TIMER_HZ        100

/* Timer interrupt period in milliseconds */
/* CFN_TIMER_PERIOD is defined in utk_config_depend.h */

/* COM1 serial port */
#define COM1_PORT       0x3F8

/* PIC ports */
#define PIC_MASTER_CMD  0x20
#define PIC_MASTER_DAT  0x21
#define PIC_SLAVE_CMD   0xA0
#define PIC_SLAVE_DAT   0xA1

/* PIT ports */
#define PIT_CH0         0x40
#define PIT_CMD         0x43

/* Max number of interrupt vectors */
#define N_INTVEC        256

#endif /* _TKDEV_CONF_ */
