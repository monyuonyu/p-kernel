/*
 * kl_main.c — p-kernel Stage-1 Loader main
 *
 * Boot sequence:
 *   1. Try FAT32 disk  → kl_fat32_load_elf()
 *   2. Try network     → kl_net_receive_elf()     (Phase 16b)
 *   3. Halt on failure
 *
 * On success, jumps to the kernel ELF entry with the original
 * Multiboot magic in EAX and Multiboot info ptr in EBX so that
 * the kernel's start.S proceeds normally.
 *
 * This file is OS-independent: no kernel headers, plain C99.
 */

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* COM1 serial (115200 8N1)                                           */
/* ------------------------------------------------------------------ */

#define COM1 0x3F8

static inline uint8_t _inb(uint16_t p)
{
    uint8_t v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "dN"(p));
    return v;
}
static inline void _outb(uint16_t p, uint8_t v)
{
    __asm__ volatile("outb %0,%1" :: "a"(v), "dN"(p));
}

static void kl_serial_init(void)
{
    _outb(COM1+1, 0x00);  /* disable interrupts */
    _outb(COM1+3, 0x80);  /* DLAB on */
    _outb(COM1+0, 0x01);  /* divisor lo: 115200 baud */
    _outb(COM1+1, 0x00);  /* divisor hi */
    _outb(COM1+3, 0x03);  /* 8N1, DLAB off */
    _outb(COM1+2, 0xC7);  /* FIFO on */
    _outb(COM1+4, 0x0B);  /* RTS+DTR */
}

static void kl_putc(char c)
{
    if (c == '\n') {
        while (!(_inb(COM1+5) & 0x20));
        _outb(COM1, '\r');
    }
    while (!(_inb(COM1+5) & 0x20));
    _outb(COM1, (uint8_t)c);
}

static void kl_puts(const char *s)
{
    while (*s) kl_putc(*s++);
}

static void kl_puthex(uint32_t v)
{
    const char *hex = "0123456789ABCDEF";
    kl_puts("0x");
    for (int i = 28; i >= 0; i -= 4)
        kl_putc(hex[(v >> i) & 0xF]);
}

/* ------------------------------------------------------------------ */
/* Forward declarations (implemented in kl_fat32.c / kl_net.c)       */
/* ------------------------------------------------------------------ */

uint32_t kl_fat32_load_elf(void);
uint32_t kl_net_receive_elf(void);

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

void kl_main(uint32_t mb_magic, uint32_t mb_info)
{
    kl_serial_init();

    kl_puts("\n");
    kl_puts("[kloader] p-kernel Stage-1 Loader\n");
    kl_puts("[kloader] MB magic=");
    kl_puthex(mb_magic);
    kl_puts("  info=");
    kl_puthex(mb_info);
    kl_puts("\n");

    uint32_t entry = 0;

    /* --- Try FAT32 disk ------------------------------------------- */
    kl_puts("[kloader] trying FAT32 disk...\n");
    entry = kl_fat32_load_elf();

    /* --- Try network (Phase 16b) ----------------------------------- */
    if (!entry) {
        kl_puts("[kloader] trying network...\n");
        entry = kl_net_receive_elf();
    }

    /* --- Give up --------------------------------------------------- */
    if (!entry) {
        kl_puts("[kloader] FATAL: no kernel found — system halted\n");
        for (;;) __asm__ volatile("hlt");
    }

    /* --- Jump to kernel ------------------------------------------- */
    kl_puts("[kloader] kernel entry=");
    kl_puthex(entry);
    kl_puts("  jumping...\n");

    /* Small delay so serial output flushes before we hand off. */
    for (volatile int i = 0; i < 2000000; i++);

    /*
     * Restore EAX = Multiboot magic, EBX = Multiboot info ptr.
     * The kernel's start.S saves them immediately on entry.
     */
    __asm__ volatile(
        "mov %0, %%eax\n"
        "mov %1, %%ebx\n"
        "jmp *%2\n"
        :: "r"(mb_magic), "r"(mb_info), "r"(entry)
        : "eax", "ebx"
    );

    /* unreachable */
    for (;;) __asm__ volatile("hlt");
}
