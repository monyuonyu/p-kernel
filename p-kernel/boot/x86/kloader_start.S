/*
 * kloader_start.S — Multiboot entry for p-kernel Stage-1 Loader
 *
 * QEMU loads kloader.bin via Multiboot:
 *   EAX = 0x2BADB002  (Multiboot magic)
 *   EBX = ptr to Multiboot info structure
 *
 * We save both and pass them to kl_main(mb_magic, mb_info).
 * kl_main in turn passes them to the kernel so start.S sees the
 * original Multiboot state and can parse the memory map normally.
 *
 * Stack: 4 KB just below kloader at 0x4F000.
 */

.code32

.set MULTIBOOT_MAGIC,    0x1BADB002
.set MULTIBOOT_FLAGS,    0x00000000
.set MULTIBOOT_CHECKSUM, -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

/* ---- Multiboot header (must be in first 8 KB of the image) ---------- */
.section .multiboot
.align 4
.long MULTIBOOT_MAGIC
.long MULTIBOOT_FLAGS
.long MULTIBOOT_CHECKSUM

/* ---- Entry point ----------------------------------------------------- */
.section .text
.global _kloader_start

_kloader_start:
    cli
    mov  $kloader_stack_top, %esp
    push %ebx           /* arg2: Multiboot info ptr */
    push %eax           /* arg1: Multiboot magic    */
    call kl_main
    /* kl_main never returns — but halt just in case */
1:  hlt
    jmp 1b

/* ---- Stack (4 KB, below kloader load address) ----------------------- */
.section .bss
.align 16
.space 4096
kloader_stack_top:
