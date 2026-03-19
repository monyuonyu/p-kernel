/*
 *  vga.c (x86)
 *  VGA text mode driver (80x25) for p-kernel
 *
 *  Writes directly to the VGA frame buffer at 0xB8000.
 *  Each cell is 2 bytes: low=char, high=attribute (bg<<4 | fg).
 */

#include "vga.h"
#include <typedef.h>

#define VGA_BUFFER    ((volatile UH *)0xB8000)
#define VGA_PORT_CMD  0x3D4
#define VGA_PORT_DATA 0x3D5

static UB vga_fg = VGA_LIGHT_GREY;
static UB vga_bg = VGA_BLACK;
static INT vga_row = 0;
static INT vga_col = 0;

static inline void outb_vga(UH port, UB val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static UH make_entry(char c, UB fg, UB bg)
{
    return (UH)(UB)c | (UH)(((UB)(bg << 4) | fg)) << 8;
}

static void update_cursor(void)
{
    UW pos = (UW)(vga_row * VGA_WIDTH + vga_col);
    outb_vga(VGA_PORT_CMD, 0x0F);
    outb_vga(VGA_PORT_DATA, (UB)(pos & 0xFF));
    outb_vga(VGA_PORT_CMD, 0x0E);
    outb_vga(VGA_PORT_DATA, (UB)((pos >> 8) & 0xFF));
}

static void scroll_up(void)
{
    UH blank = make_entry(' ', vga_fg, vga_bg);
    for (INT r = 0; r < VGA_HEIGHT - 1; r++) {
        for (INT c = 0; c < VGA_WIDTH; c++) {
            VGA_BUFFER[r * VGA_WIDTH + c] =
                VGA_BUFFER[(r + 1) * VGA_WIDTH + c];
        }
    }
    for (INT c = 0; c < VGA_WIDTH; c++) {
        VGA_BUFFER[(VGA_HEIGHT - 1) * VGA_WIDTH + c] = blank;
    }
}

void vga_init(void)
{
    vga_fg  = VGA_LIGHT_GREY;
    vga_bg  = VGA_BLACK;
    vga_row = 0;
    vga_col = 0;
    vga_clear();
}

void vga_clear(void)
{
    UH blank = make_entry(' ', vga_fg, vga_bg);
    for (INT i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_BUFFER[i] = blank;
    }
    vga_row = 0;
    vga_col = 0;
    update_cursor();
}

void vga_set_color(UB fg, UB bg)
{
    vga_fg = fg;
    vga_bg = bg;
}

void vga_putchar(char c)
{
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (c == '\r') {
        vga_col = 0;
    } else if (c == '\b') {
        if (vga_col > 0) {
            vga_col--;
            VGA_BUFFER[vga_row * VGA_WIDTH + vga_col] =
                make_entry(' ', vga_fg, vga_bg);
        }
    } else {
        VGA_BUFFER[vga_row * VGA_WIDTH + vga_col] =
            make_entry(c, vga_fg, vga_bg);
        if (++vga_col >= VGA_WIDTH) {
            vga_col = 0;
            vga_row++;
        }
    }

    if (vga_row >= VGA_HEIGHT) {
        scroll_up();
        vga_row = VGA_HEIGHT - 1;
    }
    update_cursor();
}

void vga_puts(const char *s)
{
    while (*s) {
        vga_putchar(*s++);
    }
}
