/*
 *  elf_loader.c (x86)
 *  ELF32 static executable loader
 *
 *  Parses an ELF32 binary from the VFS, copies PT_LOAD segments to their
 *  physical (= virtual, flat-mapped) addresses, then creates a ring-3
 *  T-Kernel task that IRETs into the loaded code.
 *
 *  Supported:  ET_EXEC, EM_386, PT_LOAD segments, static linking.
 *  Not supported: dynamic linking, shared libraries, ASLR.
 */

#include "kernel.h"
#include "elf_loader.h"
#include "vfs.h"
#include "gdt_user.h"
#include "userspace.h"
#include <tmonitor.h>

/* ----------------------------------------------------------------- */
/* ELF32 type definitions                                            */
/* ----------------------------------------------------------------- */

typedef UW   Elf32_Addr;
typedef UW   Elf32_Off;
typedef UH   Elf32_Half;
typedef UW   Elf32_Word;

#define EI_NIDENT    16

typedef struct __attribute__((packed)) {
    UB          e_ident[EI_NIDENT];
    Elf32_Half  e_type;
    Elf32_Half  e_machine;
    Elf32_Word  e_version;
    Elf32_Addr  e_entry;
    Elf32_Off   e_phoff;
    Elf32_Off   e_shoff;
    Elf32_Word  e_flags;
    Elf32_Half  e_ehsize;
    Elf32_Half  e_phentsize;
    Elf32_Half  e_phnum;
    Elf32_Half  e_shentsize;
    Elf32_Half  e_shnum;
    Elf32_Half  e_shstrndx;
} Elf32_Ehdr;

typedef struct __attribute__((packed)) {
    Elf32_Word  p_type;
    Elf32_Off   p_offset;
    Elf32_Addr  p_vaddr;
    Elf32_Addr  p_paddr;
    Elf32_Word  p_filesz;
    Elf32_Word  p_memsz;
    Elf32_Word  p_flags;
    Elf32_Word  p_align;
} Elf32_Phdr;

/* ELF identification */
#define ELFMAG0     0x7fu
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'
#define ELFCLASS32  1
#define ET_EXEC     2
#define EM_386      3
#define PT_LOAD     1

/* ----------------------------------------------------------------- */
/* User stack                                                        */
/* ----------------------------------------------------------------- */

/*
 * Physical user stack: a statically allocated kernel-side buffer.
 * With flat identity mapping (US bit set in PDEs) ring-3 can access
 * this by the same linear address as the kernel.
 */
static UB user_stack_buf[USER_STACK_SIZE] __attribute__((aligned(16)));

/* ----------------------------------------------------------------- */
/* Task launcher                                                     */
/* ----------------------------------------------------------------- */

/* Passed from elf_exec() to the launcher task via exinf. */
typedef struct {
    UW entry;
    UW stack_top;
} UserStartArg;

static UserStartArg _uarg;   /* single user task (no re-entrancy) */

/*
 * Ring-0 T-Kernel task that immediately IRETs into ring-3 user code.
 * Never returns — tk_ext_tsk() is called if somehow iret fails.
 */
static void user_launcher(INT stacd, void *exinf)
{
    (void)stacd;
    const UserStartArg *a = (const UserStartArg *)exinf;
    user_exec(a->entry, a->stack_top);  /* does not return */
    tk_ext_tsk();
}

/* ----------------------------------------------------------------- */
/* Hex print helper                                                  */
/* ----------------------------------------------------------------- */

static void print_hex(UW v)
{
    char buf[9];
    buf[8] = '\0';
    for (INT i = 7; i >= 0; i--) {
        INT d = (INT)(v & 0xF);
        buf[i] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        v >>= 4;
    }
    tm_putstring((UB *)buf);
}

/* ----------------------------------------------------------------- */
/* Public API                                                        */
/* ----------------------------------------------------------------- */

ID elf_exec(const char *path)
{
    /* ---- Open file ------------------------------------------------ */
    INT fd = vfs_open(path);
    if (fd < 0) {
        tm_putstring((UB *)"[elf] open failed: ");
        tm_putstring((UB *)path);
        tm_putstring((UB *)"\r\n");
        return (ID)fd;
    }

    /* ---- Read and validate ELF header ----------------------------- */
    Elf32_Ehdr ehdr;
    if (vfs_read(fd, &ehdr, sizeof(ehdr)) != (INT)sizeof(ehdr)) {
        vfs_close(fd);
        tm_putstring((UB *)"[elf] header read failed\r\n");
        return (ID)-1;
    }

    if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3 ||
        ehdr.e_ident[4] != ELFCLASS32)
    {
        vfs_close(fd);
        tm_putstring((UB *)"[elf] bad magic / not ELF32\r\n");
        return (ID)-1;
    }

    if (ehdr.e_type != ET_EXEC || ehdr.e_machine != EM_386) {
        vfs_close(fd);
        tm_putstring((UB *)"[elf] not ET_EXEC / EM_386\r\n");
        return (ID)-1;
    }

    if (ehdr.e_phnum == 0 || ehdr.e_phentsize < (Elf32_Half)sizeof(Elf32_Phdr)) {
        vfs_close(fd);
        tm_putstring((UB *)"[elf] no program headers\r\n");
        return (ID)-1;
    }

    /* ---- Load PT_LOAD segments ------------------------------------ */
    for (Elf32_Half i = 0; i < ehdr.e_phnum; i++) {
        Elf32_Off phoff = ehdr.e_phoff + (Elf32_Off)(i * sizeof(Elf32_Phdr));
        Elf32_Phdr phdr;

        if (vfs_seek(fd, phoff) < 0 ||
            vfs_read(fd, &phdr, sizeof(phdr)) != (INT)sizeof(phdr))
        {
            vfs_close(fd);
            tm_putstring((UB *)"[elf] phdr read failed\r\n");
            return (ID)-1;
        }

        if (phdr.p_type != PT_LOAD) continue;
        if (phdr.p_memsz == 0)      continue;

        UB *dest = (UB *)(UW)phdr.p_vaddr;

        /* Copy file data (p_filesz bytes) */
        if (phdr.p_filesz > 0) {
            if (vfs_seek(fd, phdr.p_offset) < 0 ||
                vfs_read(fd, dest, phdr.p_filesz) != (INT)phdr.p_filesz)
            {
                vfs_close(fd);
                tm_putstring((UB *)"[elf] segment load failed\r\n");
                return (ID)-1;
            }
        }

        /* Zero-fill BSS (p_memsz - p_filesz bytes) */
        if (phdr.p_memsz > phdr.p_filesz) {
            UB  *bss  = dest + phdr.p_filesz;
            UW   blen = phdr.p_memsz - phdr.p_filesz;
            for (UW j = 0; j < blen; j++) bss[j] = 0;
        }
    }

    vfs_close(fd);

    tm_putstring((UB *)"[elf] loaded '");
    tm_putstring((UB *)path);
    tm_putstring((UB *)"' entry=0x");
    print_hex(ehdr.e_entry);
    tm_putstring((UB *)"\r\n");

    /* ---- Prepare user stack --------------------------------------- */
    UW stack_top = (UW)user_stack_buf + USER_STACK_SIZE;
    stack_top &= ~0xFUL;   /* 16-byte align */

    /* ---- Create ring-0 launcher task ------------------------------ */
    _uarg.entry     = ehdr.e_entry;
    _uarg.stack_top = stack_top;

    T_CTSK ct;
    ct.exinf   = (void *)&_uarg;
    ct.tskatr  = TA_HLNG | TA_RNG0;   /* starts ring0, IRETs to ring3 */
    ct.task    = user_launcher;
    ct.itskpri = 8;                    /* lower than shell (priority 2) */
    ct.stksz   = 8192;

    ID tid = tk_cre_tsk(&ct);
    if (tid < E_OK) {
        tm_putstring((UB *)"[elf] tk_cre_tsk failed\r\n");
        return tid;
    }

    ER er = tk_sta_tsk(tid, 0);
    if (er < E_OK) {
        tm_putstring((UB *)"[elf] tk_sta_tsk failed\r\n");
        tk_del_tsk(tid);
        return (ID)er;
    }

    tm_putstring((UB *)"[elf] task started (tid=");
    /* print tid */
    char tbuf[8]; INT ti = 7; tbuf[ti] = '\0';
    { UW v = (UW)tid;
      if (v == 0) { tbuf[--ti] = '0'; }
      else { while (v > 0 && ti > 0) { tbuf[--ti]=(char)('0'+v%10); v/=10; } } }
    tm_putstring((UB *)(tbuf + ti));
    tm_putstring((UB *)")\r\n");

    return tid;
}
