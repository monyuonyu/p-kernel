/*
 *  arch/linux/selfc.c — self-compilation, first milestone.
 *
 *  "The OS can compile and deploy new code to itself" — until now that
 *  sentence was carried by the evolve pipeline, which outsources the
 *  compile to a mothership. This module brings the compiler INSIDE:
 *  the running kernel process embeds libtcc, compiles C source to
 *  memory (TCC_OUTPUT_MEMORY), resolves the new code against a small,
 *  explicit kernel-API table (tcc_add_symbol — we deliberately do NOT
 *  lean on -rdynamic, so compiled code can touch only what we hand it
 *  by name), and starts the entry point as a new T-Kernel task.
 *
 *  Source can come from two places:
 *    selfc demo          — a C string baked into this file
 *    selfc run <name>    — a p-fs object: code saved on ANY node in the
 *                          region replicates here via the ordinary P1
 *                          block gossip, and is then compiled and run
 *                          inside THIS node's kernel.
 *
 *  Honest limits (see docs/architecture/self-compile.md):
 *    - hosted (arch/linux) builds only; bare metal has no libtcc yet
 *    - compiled code runs with full kernel privilege — no verifier,
 *      no sandbox, no signatures yet
 *    - compiled units are never freed (their task may run forever)
 *
 *  Build: boot/linux[_x86_64] Makefiles auto-detect libtcc and add
 *  -DHAVE_LIBTCC -ltcc only when the link actually works for the
 *  target ABI. Without it this file degrades to a stub (the `selfc
 *  save` authoring verb still works — a node without a compiler can
 *  still write code for nodes that have one).
 */

#include "kernel.h"
#include <tmonitor.h>
#include "kdds.h"
#include "pfs_dag.h"
#include "pfs_block.h"
#include "selfc.h"

#ifdef HAVE_LIBTCC
#include <libtcc.h>
#include <sys/mman.h>   /* hosted TU: exec memory for the relocated code */
#endif

/* ------------------------------------------------------------------ */
/* built-in demo source                                                */
/* ------------------------------------------------------------------ */
/* Compiled at runtime inside the kernel. It declares its own
 * prototypes for the (few) kernel APIs the selfc symbol table
 * exposes; there are no headers and no libc on the other side. */

static const char selfc_demo_src[] =
    "/* This translation unit never existed as a file. It was compiled\n"
    " * by libtcc inside the running p-kernel process and started as a\n"
    " * T-Kernel task. */\n"
    "int  tm_printf(const unsigned char *fmt, ...);\n"
    "int  tk_dly_tsk(unsigned int ms);\n"
    "int  kdds_open(const char *name, int qos);\n"
    "int  kdds_pub(int h, const void *data, int len);\n"
    "\n"
    "static int checksum(const char *s)\n"
    "{\n"
    "    int v = 0;\n"
    "    while (*s) v = v * 31 + *s++;\n"
    "    return v;\n"
    "}\n"
    "\n"
    "void selfc_main(void)\n"
    "{\n"
    "    const char msg[] = \"I was compiled at runtime inside the kernel\";\n"
    "    tm_printf((const unsigned char *)\"[selfc:demo] %s\\n\", msg);\n"
    "    tm_printf((const unsigned char *)\"[selfc:demo] my own checksum says %d\\n\",\n"
    "              checksum(msg));\n"
    "    int h = kdds_open(\"selfc/born\", 0 /* BEST_EFFORT */);\n"
    "    if (h >= 0 && kdds_pub(h, msg, (int)sizeof msg) == 0)\n"
    "        tm_printf((const unsigned char *)\"[selfc:demo] published %d bytes on selfc/born\\n\",\n"
    "                  (int)sizeof msg);\n"
    "    for (int i = 1; i <= 3; i++) {\n"
    "        tm_printf((const unsigned char *)\"[selfc:demo] alive, tick %d/3\\n\", i);\n"
    "        tk_dly_tsk(200);\n"
    "    }\n"
    "    tm_printf((const unsigned char *)\"[selfc:demo] done — returning to the kernel that built me\\n\");\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* small local string helpers (no <string.h> needed)                   */
/* ------------------------------------------------------------------ */

static INT s_len(const char *s) { INT n = 0; while (s[n]) n++; return n; }

static void s_copy(char *dst, const char *src, INT max /* incl NUL */)
{
    INT i = 0;
    for (; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* token prefix match: line starts with word `b` followed by space/end */
static int tok_is(const UB *p, const UB *end, const char *b)
{
    INT i = 0;
    for (; b[i]; i++) {
        if (p + i >= end || p[i] != (UB)b[i]) return 0;
    }
    return (p + i >= end) || p[i] == ' ' || p[i] == '\t';
}

#ifdef HAVE_LIBTCC

/* ------------------------------------------------------------------ */
/* the kernel-API table compiled code may link against                 */
/* ------------------------------------------------------------------ */
/* Explicit allowlist, passed symbol by symbol via tcc_add_symbol().
 * Anything not in this table is an unresolved symbol at relocate time
 * — compiled code cannot reach into the kernel by accident. (It can
 * still scribble on memory; see the privilege note in selfc.h.) */

static const struct { const char *name; const void *fn; } selfc_api[] = {
    { "tm_printf",  (const void *)tm_printf  },   /* console output     */
    { "tk_slp_tsk", (const void *)tk_slp_tsk },   /* sleep (woken)      */
    { "tk_dly_tsk", (const void *)tk_dly_tsk },   /* delay ms           */
    { "kdds_open",  (const void *)kdds_open  },   /* pub/sub: open      */
    { "kdds_pub",   (const void *)kdds_pub   },   /* pub/sub: publish   */
    { "kdds_sub",   (const void *)kdds_sub   },   /* pub/sub: subscribe */
};
#define SELFC_API_N  ((INT)(sizeof selfc_api / sizeof selfc_api[0]))

/* ------------------------------------------------------------------ */
/* compiled-unit registry                                              */
/* ------------------------------------------------------------------ */
/* Each unit keeps its TCCState alive forever: the memory holding the
 * generated code belongs to the state, and the spawned task may run
 * for the rest of the kernel's life. 8 units is plenty for the first
 * milestone. */

#define SELFC_UNIT_MAX 8
#define SELFC_WHAT_MAX 20

typedef struct {
    TCCState *st;
    void    (*entry)(void);
    ID        tid;
    char      what[SELFC_WHAT_MAX];
} SELFC_UNIT;

static SELFC_UNIT selfc_units[SELFC_UNIT_MAX];
static INT        selfc_nunits = 0;

/* libtcc error/warning sink -> kernel console */
static void selfc_tcc_err(void *opaque, const char *msg)
{
    (void)opaque;
    tm_printf((const UB *)"[selfc] tcc: %s\n", (const UB *)msg);
}

/* task wrapper: run the compiled entry, then exit the task cleanly */
static void selfc_runner(INT stacd, void *exinf)
{
    (void)stacd;
    SELFC_UNIT *u = (SELFC_UNIT *)exinf;
    u->entry();
    tk_ext_tsk();
}

ER selfc_compile_and_run(const char *src, const char *entry_sym,
                         const char *what)
{
    if (selfc_nunits >= SELFC_UNIT_MAX) {
        tm_printf((const UB *)"[selfc] unit registry full (%d)\n",
                  SELFC_UNIT_MAX);
        return E_LIMIT;
    }
    SELFC_UNIT *u = &selfc_units[selfc_nunits];
    s_copy(u->what, what, SELFC_WHAT_MAX);

    TCCState *st = tcc_new();
    if (st == NULL) {
        tm_printf((const UB *)"[selfc] tcc_new failed\n");
        return E_NOMEM;
    }
    tcc_set_error_func(st, NULL, selfc_tcc_err);
    /* no host headers, no host libc: the API table below is the whole
     * world the compiled code can see. */
    tcc_set_options(st, "-nostdinc -nostdlib");
    tcc_set_output_type(st, TCC_OUTPUT_MEMORY);

    if (tcc_compile_string(st, src) < 0) {
        tm_printf((const UB *)"[selfc] compile FAILED (%s)\n",
                  (const UB *)u->what);
        tcc_delete(st);
        return E_PAR;
    }

    for (INT i = 0; i < SELFC_API_N; i++)
        tcc_add_symbol(st, selfc_api[i].name, selfc_api[i].fn);

    /* Two-step relocate into our own anonymous mmap instead of
     * TCC_RELOCATE_AUTO: AUTO places code on the malloc heap and then
     * mprotect(PROT_EXEC)s it, which Android/Termux SELinux denies
     * (execheap). An anonymous PROT_EXEC mapping (execmem) is allowed. */
    int codesz = tcc_relocate(st, NULL);
    if (codesz < 0) {
        tm_printf((const UB *)"[selfc] relocate(size) FAILED (%s)\n",
                  (const UB *)u->what);
        tcc_delete(st);
        return E_PAR;
    }
    void *codemem = mmap(NULL, (size_t)codesz,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (codemem == MAP_FAILED) {
        tm_printf((const UB *)"[selfc] mmap(%d, RWX) FAILED (%s)\n",
                  codesz, (const UB *)u->what);
        tcc_delete(st);
        return E_NOMEM;
    }
    if (tcc_relocate(st, codemem) < 0) {
        tm_printf((const UB *)"[selfc] relocate FAILED (%s) — "
                  "unknown symbol? only the selfc API table is linkable\n",
                  (const UB *)u->what);
        munmap(codemem, (size_t)codesz);
        tcc_delete(st);
        return E_PAR;
    }

    void (*entry)(void) = (void (*)(void))tcc_get_symbol(st, entry_sym);
    if (entry == NULL) {
        tm_printf((const UB *)"[selfc] entry '%s' not found (%s)\n",
                  (const UB *)entry_sym, (const UB *)u->what);
        munmap(codemem, (size_t)codesz);
        tcc_delete(st);
        return E_PAR;
    }

    u->st    = st;
    u->entry = entry;

    T_CTSK ct = { .exinf = u, .tskatr = TA_HLNG | TA_RNG0,
                  .task = (FP)selfc_runner, .itskpri = 6, .stksz = 8192 };
    ID tid = tk_cre_tsk(&ct);
    if (tid < E_OK) {
        tm_printf((const UB *)"[selfc] tk_cre_tsk failed (%d)\n", (W)tid);
        /* the task never started, so freeing everything is safe */
        munmap(codemem, (size_t)codesz);
        tcc_delete(st);
        u->st = NULL;
        return (ER)tid;
    }
    u->tid = tid;
    selfc_nunits++;

    tm_printf((const UB *)"[selfc] '%s' compiled in-process, "
              "entry %s -> task %d started\n",
              (const UB *)u->what, (const UB *)entry_sym, (W)tid);
    tk_sta_tsk(tid, 0);
    return (ER)tid;
}

static void selfc_ls(void)
{
    tm_printf((const UB *)"[selfc] %d unit(s) compiled this boot:\n",
              selfc_nunits);
    for (INT i = 0; i < selfc_nunits; i++)
        tm_printf((const UB *)"  #%d  task %d  %s\n",
                  i, (W)selfc_units[i].tid, (const UB *)selfc_units[i].what);
}

#else /* !HAVE_LIBTCC ------------------------------------------------ */

ER selfc_compile_and_run(const char *src, const char *entry_sym,
                         const char *what)
{
    (void)src; (void)entry_sym; (void)what;
    tm_printf((const UB *)"[selfc] this build has no libtcc "
              "(HAVE_LIBTCC off) — cannot self-compile.\n"
              "[selfc] install libtcc-dev and rebuild boot/linux.\n");
    return E_NOSPT;
}

static void selfc_ls(void)
{
    tm_printf((const UB *)"[selfc] no libtcc in this build — 0 units\n");
}

#endif /* HAVE_LIBTCC */

/* ------------------------------------------------------------------ */
/* shell command                                                       */
/* ------------------------------------------------------------------ */

/* scratch for `selfc run` — p-fs objects are <= PFS_BLOCK_MAX bytes,
 * static so it never burdens the shell task's stack
 * (feedback_hosted_relay_stack_overflow) */
static UB selfc_srcbuf[PFS_BLOCK_MAX + 1];

static void selfc_usage(void)
{
    tm_printf((const UB *)
        "usage: selfc demo            compile+run the built-in demo\n"
        "       selfc save <name>     save demo C source to p-fs <name>\n"
        "       selfc run <name>      compile+run C source from p-fs\n"
        "       selfc ls              list compiled units\n");
}

/* extract a <name> token into nm (NUL-terminated); returns len or 0 */
static INT selfc_token(const UB **pp, const UB *end, char *nm, INT max)
{
    const UB *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    INT i = 0;
    while (p < end && *p != ' ' && *p != '\t' && i < max - 1)
        nm[i++] = (char)*p++;
    nm[i] = '\0';
    *pp = p;
    return i;
}

void selfc_cmd(const UB *line, INT n)
{
    const UB *p   = line + 5;          /* skip "selfc" */
    const UB *end = line + n;
    while (p < end && (*p == ' ' || *p == '\t')) p++;

    if (p < end && tok_is(p, end, "demo")) {
        selfc_compile_and_run(selfc_demo_src, "selfc_main", "demo");

    } else if (p < end && tok_is(p, end, "save")) {
        p += 4;
        char nm[PFS_NAME_MAX + 1];
        INT  nl = selfc_token(&p, end, nm, sizeof nm);
        if (nl == 0) { selfc_usage(); return; }
        INT r = pfs_dag_save((const UB *)nm, (UW)nl,
                             selfc_demo_src, (UW)s_len(selfc_demo_src));
        if (r == PFS_OK)
            tm_printf((const UB *)"[selfc] demo source (%d bytes of C) "
                      "saved as p-fs object '%s' — it now replicates "
                      "like any block\n", s_len(selfc_demo_src),
                      (const UB *)nm);
        else
            tm_printf((const UB *)"[selfc] pfs save failed (%d)\n", r);

    } else if (p < end && tok_is(p, end, "run")) {
        p += 3;
        char nm[PFS_NAME_MAX + 1];
        INT  nl = selfc_token(&p, end, nm, sizeof nm);
        if (nl == 0) { selfc_usage(); return; }
        /* first read may only issue a P1 WANT for a block that is
         * still on its way — retry a few times before giving up */
        INT len = -1;
        for (INT t = 0; t < 6; t++) {
            len = pfs_dag_read((const UB *)nm, (UW)nl,
                               selfc_srcbuf, PFS_BLOCK_MAX);
            if (len >= 0) break;
            tk_dly_tsk(400);
        }
        if (len < 0) {
            tm_printf((const UB *)"[selfc] p-fs object '%s' not found "
                      "(%d) — not saved yet, or still replicating\n",
                      (const UB *)nm, len);
            return;
        }
        selfc_srcbuf[len] = '\0';
        tm_printf((const UB *)"[selfc] %d bytes of C read from p-fs "
                  "'%s' — compiling in-process\n", len, (const UB *)nm);
        selfc_compile_and_run((const char *)selfc_srcbuf, "selfc_main", nm);

    } else if (p < end && tok_is(p, end, "ls")) {
        selfc_ls();

    } else {
        selfc_usage();
    }
}
