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
#include "paging.h"
#include "userspace.h"
#include "paging.h"
#include "user_range.h"   /* ISO-USERPTR — validate segment placement */
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
/* Task launcher                                                     */
/* ----------------------------------------------------------------- */

/* Passed from elf_exec() to the launcher task via exinf. */
typedef struct {
    UW entry;
    UW stack_top;
    UW gs_sel;    /* GS selector: USER_DS or USER_TLS_SEL */
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
    user_exec(a->entry, a->stack_top, a->gs_sel);  /* does not return */
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

/* ----------------------------------------------------------------- */
/* argv builder — writes argc/argv/envp/auxv onto the user stack     */
/* ----------------------------------------------------------------- */

#define ARGV_MAX     32
#define ARGV_STRBUF  512   /* total string storage on stack */

/*
 * Parse cmdline into tokens and lay out a Linux-style initial stack
 * frame.  Returns the new ESP (pointing to argc).
 *
 * Stack layout built here (grows downward, ESP = lowest address):
 *   [esp+0]            argc
 *   [esp+4..4*argc+4]  argv[0..argc-1] pointers
 *   [esp+4*(argc+1)]   NULL  (end of argv)
 *   [esp+4*(argc+2)]   NULL  (end of envp)
 *   [esp+4*(argc+3)]   0     (AT_NULL type)
 *   [esp+4*(argc+4)]   0     (AT_NULL value)
 *   ... (strings stored above, high addresses)
 */
static UW build_argv_stack(const char *cmdline, UW stack_top)
{
    /* ---- tokenise cmdline ---- */
    static char strbuf[ARGV_STRBUF];
    const char *toks[ARGV_MAX];
    UB  tlen[ARGV_MAX];
    INT argc = 0;

    INT spos = 0;
    const char *p = cmdline ? cmdline : "";
    while (*p && argc < ARGV_MAX) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') p++;
        INT l = (INT)(p - start);
        if (l > 127) l = 127;
        if (spos + l + 1 > ARGV_STRBUF) break;
        for (INT k = 0; k < l; k++) strbuf[spos + k] = start[k];
        strbuf[spos + l] = '\0';
        toks[argc] = strbuf + spos;
        tlen[argc] = (UB)l;
        argc++;
        spos += l + 1;
    }

    /* ---- lay out strings starting just below stack_top ---- */
    UW sp = stack_top;

    /* Copy strings from high to low address */
    UW str_addrs[ARGV_MAX];
    for (INT i = argc - 1; i >= 0; i--) {
        INT l = tlen[i] + 1;           /* include NUL */
        sp -= (UW)l;
        sp &= ~3UL;                     /* 4-byte align */
        UB *dst = (UB *)sp;
        for (INT k = 0; k <= tlen[i]; k++) dst[k] = (UB)toks[i][k];
        str_addrs[i] = sp;
    }
    sp &= ~15UL;    /* 16-byte align before pointer array */

    /* ---- auxv AT_NULL (2 × UW = 8 bytes) ---- */
    sp -= 4; *(UW *)sp = 0;  /* AT_NULL value */
    sp -= 4; *(UW *)sp = 0;  /* AT_NULL type  */

    /* ---- envp NULL terminator ---- */
    sp -= 4; *(UW *)sp = 0;

    /* ---- argv pointers (NULL terminator then pointers, reversed) ---- */
    sp -= 4; *(UW *)sp = 0;             /* argv[argc] = NULL */
    for (INT i = argc - 1; i >= 0; i--) {
        sp -= 4;
        *(UW *)sp = str_addrs[i];
    }

    /* ---- argc ---- */
    sp -= 4; *(UW *)sp = (UW)argc;

    return sp;  /* new ESP */
}

/* ----------------------------------------------------------------- */
/* elf_exec                                                          */
/* ----------------------------------------------------------------- */

ID elf_exec(const char *path, const char *cmdline)
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
    UW brk_start = 0;   /* track highest loaded address for brk init */
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

        /* ISO-USERPTR: a malformed ELF could target a segment at a KERNEL
         * address; the loader would then blindly copy file bytes / zero-fill
         * there.  Require [p_vaddr, p_vaddr+p_memsz) to land entirely inside
         * a ring-3-accessible region before writing.  (p_filesz <= p_memsz
         * is enforced too, so the file-copy span is covered by the memsz
         * check.) */
        if (phdr.p_filesz > phdr.p_memsz ||
            !user_range_ok((const void *)(UW)phdr.p_vaddr, phdr.p_memsz)) {
            vfs_close(fd);
            tm_putstring((UB *)"[elf] segment out of user range (rejected)\r\n");
            return (ID)-1;
        }

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

        /* Track highest address for initial brk */
        UW seg_end = phdr.p_vaddr + phdr.p_memsz;
        if (seg_end > brk_start) brk_start = seg_end;
    }

    vfs_close(fd);

    tm_putstring((UB *)"[elf] loaded '");
    tm_putstring((UB *)path);
    tm_putstring((UB *)"' entry=0x");
    print_hex(ehdr.e_entry);
    tm_putstring((UB *)"\r\n");

    /* ---- Allocate per-process page tables ------------------------- */
    UW proc_cr3 = paging_proc_create();
    if (!proc_cr3) {
        tm_putstring((UB *)"[elf] paging_proc_create failed\r\n");
        return (ID)-1;
    }

    /* ---- Prepare user stack --------------------------------------- */
    /* Linux-standard ELFs load at 0x08048000; use a high stack area.
     * p-kernel native ELFs load at 0x400000; use USER_STACK_TOP.    */
    UW stack_top;
    if (ehdr.e_entry >= 0x08000000UL) {
        /* Linux-compatible binary (musl/glibc).
         * Build a proper Linux initial stack: argc, argv[], envp[], auxv.
         * Strings and pointers are stored just below stack top (0x0FFF000).
         * build_argv_stack() returns the new ESP (= address of argc). */
        stack_top = build_argv_stack(cmdline, 0x0FFF000UL);

        /* Set up minimal TLS block for musl (gs:0 = self-pointer).
         * Placed at 0x08060000 (PD[64], mapped for ring-3).
         * musl overwrites this via set_thread_area once it allocates TLS. */
        UW tls_addr = 0x08060000UL;
        UW *tls = (UW *)tls_addr;
        tls[0] = tls_addr;   /* gs:0 = self-pointer (pthread_self) */
        tls[1] = 0;
        tls[2] = 0;
        tls[3] = 0;
        gdt_set_user_tls(tls_addr);
    } else {
        /* ring3-core Wave C (III.1.4): native p-kernel ELFs also get the
         * Linux-style argc/argv frame — core_mind.elf selects its mode
         * (-poison / -crash) by argv.  Existing native samples' _start
         * never reads the stack, so they are unaffected (ESP just starts
         * a few words lower, with the frame above it). */
        stack_top = build_argv_stack(cmdline, USER_STACK_TOP);
    }

    /* ---- Create ring-0 launcher task ------------------------------ */
    _uarg.entry     = ehdr.e_entry;
    _uarg.stack_top = stack_top;
    _uarg.gs_sel    = (ehdr.e_entry >= 0x08000000UL)
                      ? USER_TLS_SEL : (UW)USER_DS;

    T_CTSK ct;
    ct.exinf   = (void *)&_uarg;
    ct.tskatr  = TA_HLNG | TA_RNG0;   /* starts ring0, IRETs to ring3 */
    ct.task    = user_launcher;
    ct.itskpri = 8;                    /* lower than shell (priority 2) */
    ct.stksz   = 8192;

    ID tid = tk_cre_tsk(&ct);
    if (tid < E_OK) {
        paging_proc_destroy(proc_cr3);
        tm_putstring((UB *)"[elf] tk_cre_tsk failed\r\n");
        return tid;
    }

    /* Register process CR3 and initial brk before starting the task */
    paging_set_task_cr3(tid, proc_cr3);
    /* Align brk to 4KB boundary */
    paging_set_task_brk(tid, (brk_start + 0xFFFUL) & ~0xFFFUL);

    ER er = tk_sta_tsk(tid, 0);
    if (er < E_OK) {
        paging_set_task_cr3(tid, 0);
        paging_proc_destroy(proc_cr3);
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
