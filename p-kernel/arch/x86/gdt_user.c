/*
 *  gdt_user.c (x86)
 *  Runtime GDT extension for ring-3 user space (IA-32e mode)
 *
 *  The bootloader (start.S) loads a minimal GDT with 8 placeholder slots:
 *    [0] null  [1] kern code  [2] kern data  [3] 64-bit code
 *    [4..7]    reserved (will be overwritten here at C runtime)
 *
 *  We extend it here (at C runtime) by:
 *    [4] ring3 code  (0x20, DPL=3)
 *    [5] ring3 data  (0x28, DPL=3)
 *    [6..7] 64-bit TSS  (0x30, requires 16 bytes = 2 slots in IA-32e mode)
 *
 *  Steps:
 *    1. sgdt → get current GDT base and limit
 *    2. Write descriptors [4][5] at GDT_BASE + 0x20/0x28
 *    3. Write 64-bit TSS descriptor at [6..7] (GDT_BASE + 0x30)
 *    4. lgdt with new limit (8 * 8 - 1 = 63)
 *    5. ltr 0x30
 */

#include "kernel.h"
#include "gdt_user.h"
#include <tmonitor.h>

/* ----------------------------------------------------------------- */
/* 64-bit TSS (IA-32e mode — only RSP0 is needed for ring3→ring0)    */
/*                                                                     */
/* Intel SDM Vol.3 §7.7: In IA-32e mode the TSS is 104 bytes min.    */
/* The CPU reads RSP0 from offset 4 as a 64-bit value when switching  */
/* from ring3 to ring0 on an interrupt/exception/syscall.              */
/* ----------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    UW  reserved0;          /* offset  0 (4 bytes) */
    UW  rsp0_lo;            /* offset  4: RSP0 low  32-bits */
    UW  rsp0_hi;            /* offset  8: RSP0 high 32-bits (0 for <4 GB) */
    UW  rsp1_lo; UW rsp1_hi;
    UW  rsp2_lo; UW rsp2_hi;
    UW  reserved1; UW reserved2;
    /* IST1..7 (Interrupt Stack Table, not used) */
    UW  ist1_lo; UW ist1_hi;
    UW  ist2_lo; UW ist2_hi;
    UW  ist3_lo; UW ist3_hi;
    UW  ist4_lo; UW ist4_hi;
    UW  ist5_lo; UW ist5_hi;
    UW  ist6_lo; UW ist6_hi;
    UW  ist7_lo; UW ist7_hi;
    UW  reserved3; UW reserved4;
    UH  reserved5;
    UH  iomap_base;         /* I/O permission bitmap offset */
} TSS64;

/* sizeof(TSS64) = 4 + 8+8+8 + 8 + 7*8 + 8 + 2+2 = 104 bytes */

static TSS64 kernel_tss __attribute__((aligned(16)));

/* ----------------------------------------------------------------- */
/* GDT descriptor helpers                                             */
/* ----------------------------------------------------------------- */

static UW  gdt_base  = 0;

/*
 * Pack the low 32 bits of a segment descriptor:
 *   bits[15:0]  = limit[15:0]
 *   bits[31:16] = base[15:0]
 */
static UW desc_lo(UW base, UW limit)
{
    return (limit & 0xFFFF) | ((base & 0xFFFF) << 16);
}

/*
 * Pack the high 32 bits of a segment descriptor:
 *   bits[ 7: 0] = base[23:16]
 *   bits[15: 8] = access byte
 *   bits[19:16] = limit[19:16]
 *   bits[23:20] = flags nibble (G, D/B, L, AVL)
 *   bits[31:24] = base[31:24]
 */
static UW desc_hi(UW base, UW limit, UB access, UB flags)
{
    /*
     * High dword of an 8-byte GDT segment descriptor (bytes 4..7):
     *   byte 4  = base[23:16]   → bits  7: 0
     *   byte 5  = access byte   → bits 15: 8
     *   byte 6  = limit[19:16]  → bits 19:16
     *             flags (G/DB/L/AVL) → bits 23:20
     *   byte 7  = base[31:24]   → bits 31:24
     */
    return ((base >> 16) & 0xFF)              /* bits  7: 0 = base[23:16] */
         | ((UW)access << 8)                  /* bits 15: 8 = access byte */
         | (((limit >> 16) & 0x0F) << 16)     /* bits 19:16 = limit[19:16] */
         | ((UW)(flags & 0x0F) << 20)         /* bits 23:20 = G/DB/L/AVL  */
         | (((base >> 24) & 0xFF) << 24);     /* bits 31:24 = base[31:24] */
}

/* Write a 64-bit descriptor at GDT slot `index` (0-based). */
static void write_desc(INT index, UW lo, UW hi)
{
    UW *slot = (UW *)(gdt_base + (UW)(index * 8));
    slot[0] = lo;
    slot[1] = hi;
}

/* ----------------------------------------------------------------- */
/* Public API                                                         */
/* ----------------------------------------------------------------- */

void gdt_init_userspace(void)
{
    /* 1. Retrieve current GDT descriptor */
    struct __attribute__((packed)) { UH limit; UW base; } gdtr;
    asm volatile("sgdt %0" : "=m"(gdtr));
    gdt_base = gdtr.base;

    /* 2. Ring-3 code descriptor at slot 4 (selector 0x20)
     *    base=0, limit=4 GB, G=1, D=1, P=1, DPL=3, type=code exec/read
     *    access=0xFA: P=1 DPL=11 S=1 type=1010
     *    flags =0xC : G=1 D/B=1 */
    write_desc(4,
        desc_lo(0, 0xFFFFF),
        desc_hi(0, 0xFFFFF, 0xFA, 0xC));

    /* 3. Ring-3 data descriptor at slot 5 (selector 0x28)
     *    base=0, limit=4 GB, G=1, D=1, P=1, DPL=3, type=data r/w
     *    access=0xF2: P=1 DPL=11 S=1 type=0010
     *    flags =0xC : G=1 D/B=1 */
    write_desc(5,
        desc_lo(0, 0xFFFFF),
        desc_hi(0, 0xFFFFF, 0xF2, 0xC));

    /* 4. 64-bit TSS descriptor at slots 6+7 (selector 0x30)
     *
     *    In IA-32e mode the TSS descriptor is 16 bytes (two GDT slots):
     *      slot 6 (lower 8 bytes): standard GDT descriptor bits
     *      slot 7 (upper 8 bytes): bits[63:32] of base address (zero for
     *                              kernels loaded below 4 GB)
     *
     *    access=0x89: P=1 DPL=0 S=0 type=1001
     *      → "64-bit TSS Available" in IA-32e mode (Intel SDM §3.5 Table 3-2)
     *    flags =0x0 : no granularity/DB bits for system segment */
    UW tss_base  = (UW)&kernel_tss;
    UW tss_limit = (UW)(sizeof(kernel_tss) - 1);

    write_desc(6,
        desc_lo(tss_base, tss_limit),
        desc_hi(tss_base, tss_limit, 0x89, 0x0));

    /* Upper half of 16-byte TSS descriptor: base[63:32] = 0 */
    write_desc(7, 0, 0);

    /* 5. Reload GDT covering 8 descriptors (slots 0..7), limit = 63 */
    struct __attribute__((packed)) { UH limit; UW base; } new_gdtr;
    new_gdtr.limit = 8 * 8 - 1;  /* 63 */
    new_gdtr.base  = gdt_base;
    asm volatile("lgdt %0" : : "m"(new_gdtr));

    /* 6. Initialise TSS: zero everything, then set iomap_base */
    for (UW i = 0; i < sizeof(kernel_tss); i++) ((UB *)&kernel_tss)[i] = 0;
    /* iomap_base >= sizeof(TSS) disables the I/O permission bitmap */
    kernel_tss.iomap_base = (UH)sizeof(kernel_tss);

    /* 7. Load TSS selector */
    asm volatile("ltr %0" : : "r"((UH)TSS_SEL));

    tm_putstring((UB *)"[gdt]  ring3 segments + 64-bit TSS loaded\r\n");
}

void gdt_set_kernel_stack(UW esp0)
{
    kernel_tss.rsp0_lo = esp0;
    kernel_tss.rsp0_hi = 0;   /* kernel lives below 4 GB */
}
