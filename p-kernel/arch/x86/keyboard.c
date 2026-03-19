/*
 *  keyboard.c (x86)
 *  PS/2 keyboard driver for p-kernel
 *
 *  Handles IRQ1, converts US scancode set 1 to ASCII,
 *  stores characters in a ring buffer and signals a
 *  T-Kernel semaphore so shell_task() can block cleanly.
 */

#include "keyboard.h"
#include "kernel.h"

#define KBD_DATA_PORT  0x60
#define KBD_BUF_SIZE   64

/* Ring buffer */
static volatile UB  kbd_buf[KBD_BUF_SIZE];
static volatile INT kbd_head = 0;
static volatile INT kbd_tail = 0;

/* T-Kernel semaphore to wake shell task */
static ID kbd_sem = 0;

static inline UB inb_kbd(UH port)
{
    UB ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb_kbd(UH port, UB val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/*
 * US keyboard scancode set 1 → ASCII
 * Index = make code (0x01–0x53); 0 = not printable
 */
static const char scancode_ascii[128] = {
    /*00*/  0,
    /*01*/  27,                         /* Esc */
    /*02-0B*/ '1','2','3','4','5','6','7','8','9','0',
    /*0C-0D*/ '-','=',
    /*0E*/  '\b',                       /* Backspace */
    /*0F*/  '\t',                       /* Tab */
    /*10-19*/ 'q','w','e','r','t','y','u','i','o','p',
    /*1A-1B*/ '[',']',
    /*1C*/  '\n',                       /* Enter */
    /*1D*/  0,                          /* Left Ctrl */
    /*1E-26*/ 'a','s','d','f','g','h','j','k','l',
    /*27-29*/ ';','\'','`',
    /*2A*/  0,                          /* Left Shift */
    /*2B*/  '\\',
    /*2C-32*/ 'z','x','c','v','b','n','m',
    /*33-35*/ ',','.','/',
    /*36*/  0,                          /* Right Shift */
    /*37*/  '*',
    /*38*/  0,                          /* Left Alt */
    /*39*/  ' ',
    /* rest: function keys, etc. — not printable */
};

/* IRQ dispatch table defined in idt.c */
IMPORT void (*x86_irq_handlers[16])(void);

/*
 * kbd_irq_handler - called from IRQ1 dispatcher (interrupt context)
 */
static void kbd_irq_handler(void)
{
    UB sc = inb_kbd(KBD_DATA_PORT);

    /* Ignore break codes (key release: bit 7 set) */
    if (sc & 0x80) return;
    if (sc >= 128)  return;

    char c = scancode_ascii[sc];
    if (c == 0) return;

    /* Push to ring buffer */
    INT next = (kbd_tail + 1) % KBD_BUF_SIZE;
    if (next == kbd_head) return;   /* buffer full — drop */

    kbd_buf[kbd_tail] = (UB)c;
    kbd_tail = next;

    /* Wake shell task */
    if (kbd_sem > 0) {
        tk_sig_sem(kbd_sem, 1);
    }
}

void kbd_init(ID sem)
{
    kbd_sem  = sem;
    kbd_head = 0;
    kbd_tail = 0;

    /* Register IRQ1 handler */
    x86_irq_handlers[1] = kbd_irq_handler;

    /* Unmask IRQ1 in PIC master mask (port 0x21) */
    UB mask = inb_kbd(0x21);
    outb_kbd(0x21, (UB)(mask & ~0x02));
}

char kbd_getchar(void)
{
    tk_wai_sem(kbd_sem, 1, TMO_FEVR);
    char c = (char)kbd_buf[kbd_head];
    kbd_head = (kbd_head + 1) % KBD_BUF_SIZE;
    return c;
}

INT kbd_available(void)
{
    return kbd_head != kbd_tail;
}
