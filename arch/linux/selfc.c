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
#include "lm_self.h"
#include "selfc.h"
#include "selfc_proc.h"

#ifdef HAVE_LIBTCC
#include <libtcc.h>
#include <sys/mman.h>   /* hosted TU: exec memory for the relocated code */
#include <stdlib.h>     /* getenv — runtime SELFC_ISOLATE override        */
#include <string.h>     /* strncmp / memcmp                               */
#include <signal.h>     /* SIGSEGV — the [selfc-isolated] reap comparison */
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

/* LEGACY (in-kernel-task) table — full kernel privilege, same address
 * space. Kept ONLY for the disease phase (SELFC_ISOLATE=0): this is the
 * binding that lets a crashing unit kill the node (selfc-ring3.md §0.1). */
static const struct { const char *name; const void *fn; } selfc_api_legacy[] = {
    { "tm_printf",  (const void *)tm_printf  },   /* console output     */
    { "tk_slp_tsk", (const void *)tk_slp_tsk },   /* sleep (woken)      */
    { "tk_dly_tsk", (const void *)tk_dly_tsk },   /* delay ms           */
    { "kdds_open",  (const void *)kdds_open  },   /* pub/sub: open      */
    { "kdds_pub",   (const void *)kdds_pub   },   /* pub/sub: publish   */
    { "kdds_sub",   (const void *)kdds_sub   },   /* pub/sub: subscribe */
};
#define SELFC_API_LEGACY_N  ((INT)(sizeof selfc_api_legacy / sizeof selfc_api_legacy[0]))

/* ISOLATED (germ-process) table — the v1 capability set (selfc-ring3.md
 * §1.2). The same SYMBOL NAMES bind to PROXY stubs that marshal over the
 * socketpair to the parent (the hosted mirror of ring3's int 0x80).
 *   - tm_printf -> SELFC_SYS_LOG (rate-limited, [unit:<name>] prefix)
 *   - tk_dly_tsk -> child-local clock_nanosleep (no round-trip)
 *   - kdds_open/pub/sub -> SELFC_SYS_OPEN/PUB/SUB (topic confined to unit/<name>/)
 *   - tk_slp_tsk is REMOVED (no cross-process wakeup; a capability SHRINK).
 * A unit that names any other symbol (e.g. pfs_dag_save) is an UNRESOLVED
 * symbol at relocate time — it cannot even LINK against p-fs (the link-time
 * half of the capability boundary; the parent dispatcher is the runtime
 * half). */
static const struct { const char *name; const void *fn; } selfc_api_isolated[] = {
    { "tm_printf",  (const void *)selfc_proxy_printf    },
    { "tk_dly_tsk", (const void *)selfc_proxy_dly_tsk   },
    { "kdds_open",  (const void *)selfc_proxy_kdds_open },
    { "kdds_pub",   (const void *)selfc_proxy_kdds_pub  },
    { "kdds_sub",   (const void *)selfc_proxy_kdds_sub  },
};
#define SELFC_API_ISOLATED_N ((INT)(sizeof selfc_api_isolated / sizeof selfc_api_isolated[0]))

/* Isolation default is ON (SELFC_ISOLATE compiled-in 1); a runtime env var
 * SELFC_ISOLATE=0 forces the legacy in-task path — used ONLY by the disease
 * phase of the [selfc-isolated] gate (selfc-ring3.md §5.1). */
#ifndef SELFC_ISOLATE
#define SELFC_ISOLATE 1
#endif
static int selfc_isolation_on(void)
{
    const char *e = getenv("SELFC_ISOLATE");
    if (e && e[0] == '0') return 0;        /* explicit disease-phase opt-out */
    return SELFC_ISOLATE ? 1 : 0;
}

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

/* ------------------------------------------------------------------ */
/* compile + relocate + resolve entry (the REUSED path; selfc-ring3.md  */
/* §6: "the fork point is inserted AFTER entry resolution; one compiler  */
/* integration in the tree, ever"). `isolated` selects the API table:    */
/* the isolated table binds the proxy stubs (for the germ child) — note  */
/* the compiled image is the SAME whether run in-task or in a germ; only */
/* the symbol BINDING differs. Returns the entry pointer (registered in  */
/* the unit table) or NULL on any failure (prints why).                  */
/* ------------------------------------------------------------------ */

static void (*selfc_compile_resolve(const char *src, const char *entry_sym,
                                    const char *what, int isolated))(void)
{
    if (selfc_nunits >= SELFC_UNIT_MAX) {
        tm_printf((const UB *)"[selfc] unit registry full (%d)\n", SELFC_UNIT_MAX);
        return NULL;
    }
    SELFC_UNIT *u = &selfc_units[selfc_nunits];
    s_copy(u->what, what, SELFC_WHAT_MAX);

    TCCState *st = tcc_new();
    if (st == NULL) { tm_printf((const UB *)"[selfc] tcc_new failed\n"); return NULL; }
    tcc_set_error_func(st, NULL, selfc_tcc_err);
    /* no host headers, no host libc: the API table below is the whole
     * world the compiled code can see. */
    tcc_set_options(st, "-nostdinc -nostdlib");
    tcc_set_output_type(st, TCC_OUTPUT_MEMORY);

    if (tcc_compile_string(st, src) < 0) {
        tm_printf((const UB *)"[selfc] compile FAILED (%s)\n", (const UB *)u->what);
        tcc_delete(st);
        return NULL;
    }

    if (isolated)
        for (INT i = 0; i < SELFC_API_ISOLATED_N; i++)
            tcc_add_symbol(st, selfc_api_isolated[i].name, selfc_api_isolated[i].fn);
    else
        for (INT i = 0; i < SELFC_API_LEGACY_N; i++)
            tcc_add_symbol(st, selfc_api_legacy[i].name, selfc_api_legacy[i].fn);

    /* Two-step relocate into our own anonymous mmap instead of
     * TCC_RELOCATE_AUTO: AUTO places code on the malloc heap and then
     * mprotect(PROT_EXEC)s it, which Android/Termux SELinux denies
     * (execheap). An anonymous PROT_EXEC mapping (execmem) is allowed.
     * The mapping is created BEFORE any fork, so the germ child inherits
     * the executable image via COW with zero re-compilation (§2.1). */
    int codesz = tcc_relocate(st, NULL);
    if (codesz < 0) {
        tm_printf((const UB *)"[selfc] relocate(size) FAILED (%s)\n", (const UB *)u->what);
        tcc_delete(st);
        return NULL;
    }
    void *codemem = mmap(NULL, (size_t)codesz,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (codemem == MAP_FAILED) {
        tm_printf((const UB *)"[selfc] mmap(%d, RWX) FAILED (%s)\n",
                  codesz, (const UB *)u->what);
        tcc_delete(st);
        return NULL;
    }
    if (tcc_relocate(st, codemem) < 0) {
        tm_printf((const UB *)"[selfc] relocate FAILED (%s) — "
                  "unknown symbol? only the selfc API table is linkable "
                  "(a unit naming p-fs has no symbol to bind — §1.2)\n",
                  (const UB *)u->what);
        munmap(codemem, (size_t)codesz);
        tcc_delete(st);
        return NULL;
    }

    void (*entry)(void) = (void (*)(void))tcc_get_symbol(st, entry_sym);
    if (entry == NULL) {
        tm_printf((const UB *)"[selfc] entry '%s' not found (%s)\n",
                  (const UB *)entry_sym, (const UB *)u->what);
        munmap(codemem, (size_t)codesz);
        tcc_delete(st);
        return NULL;
    }

    u->st    = st;
    u->entry = entry;
    u->tid   = -1;
    selfc_nunits++;
    return entry;
}

/* LEGACY in-kernel-task path (selfc-ring3.md §0.1): start the entry as an
 * ordinary TA_RNG0 task, NOT guard-registered — a crash takes the node
 * down. This is the DISEASE binding, kept exactly so the [selfc-isolated]
 * gate can capture the real death (SELFC_ISOLATE=0 / `selfc demo`). */
ER selfc_compile_and_run(const char *src, const char *entry_sym,
                         const char *what)
{
    void (*entry)(void) = selfc_compile_resolve(src, entry_sym, what, 0 /* legacy */);
    if (!entry) return E_PAR;
    SELFC_UNIT *u = &selfc_units[selfc_nunits - 1];

    T_CTSK ct = { .exinf = u, .tskatr = TA_HLNG | TA_RNG0,
                  .task = (FP)selfc_runner, .itskpri = 6, .stksz = 8192 };
    ID tid = tk_cre_tsk(&ct);
    if (tid < E_OK) {
        tm_printf((const UB *)"[selfc] tk_cre_tsk failed (%d)\n", (W)tid);
        u->st = NULL;
        return (ER)tid;
    }
    u->tid = tid;
    tm_printf((const UB *)"[selfc] '%s' compiled in-process, "
              "entry %s -> task %d started (LEGACY in-kernel — a crash here "
              "kills the node; isolation OFF)\n",
              (const UB *)u->what, (const UB *)entry_sym, (W)tid);
    tk_sta_tsk(tid, 0);
    return (ER)tid;
}

/* ------------------------------------------------------------------ */
/* selfc_resolve_unit — read unit/<name>@seq source from p-fs, compile  */
/* it ISOLATED, return the entry. Used by `selfc run`'s germ path and by */
/* the supervisor's rollback (selfc_proc.c). The TCCState/RWX image is   */
/* registered in the unit table and inherited by the germ child via COW. */
/* ------------------------------------------------------------------ */

static UB selfc_resolve_buf[PFS_BLOCK_MAX + 1];

/* a unit's short NAME (e.g. "gate") is the supervisor key + the topic
 * namespace (unit/<name>/); its SOURCE lives at p-fs ref "unit/<name>"
 * (selfc-ring3.md §1.1). This builds the ref from the short name. */
static INT selfc_unit_ref(const char *name, char *out, INT max)
{
    INT i = 0;
    const char *pfx = "unit/";
    for (INT k = 0; pfx[k] && i < max - 1; k++) out[i++] = pfx[k];
    for (INT k = 0; name[k] && i < max - 1; k++) out[i++] = name[k];
    out[i] = '\0';
    return i;
}

void *selfc_resolve_unit(const char *name, U4 seq)
{
    char ref[PFS_NAME_MAX + 1];
    INT  rl = selfc_unit_ref(name, ref, sizeof ref);
    INT len = -1;
    for (INT t = 0; t < 6; t++) {
        len = pfs_dag_read_at((const UB *)ref, (UW)rl, (UW)seq,
                              selfc_resolve_buf, PFS_BLOCK_MAX);
        if (len >= 0) break;
        tk_dly_tsk(400);                  /* block may still be replicating */
    }
    if (len < 0) {
        tm_printf((const UB *)"[selfc] resolve '%s'@%u: source not available (%d)\n",
                  (const UB *)ref, (unsigned)seq, (W)len);
        return NULL;
    }
    selfc_resolve_buf[len] = '\0';
    return (void *)selfc_compile_resolve((const char *)selfc_resolve_buf,
                                         "selfc_main", name, 1 /* isolated */);
}

/* ------------------------------------------------------------------ */
/* unit germ path: read the head version of unit/<name>, then germinate */
/* ------------------------------------------------------------------ */

/* read the head manifest seq of unit/<name> (its current version), or 0. */
static U4 selfc_head_seq(const char *name)
{
    static UB tmp[PFS_BLOCK_MAX];          /* static: keep off the task stack */
    char ref[PFS_NAME_MAX + 1];
    INT  rl = selfc_unit_ref(name, ref, sizeof ref);
    /* probe successive seqs from 1 up using read_at; the highest that
     * resolves is the head. Bounded by PFSD_LOG_MAX-ish small chains. */
    U4 hi = 0;
    for (U4 s = 1; s <= 64; s++) {
        INT r = pfs_dag_read_at((const UB *)ref, (UW)rl, (UW)s,
                                tmp, sizeof tmp);
        if (r >= 0) hi = s;
        else if (s > 1) break;            /* chain exhausted */
    }
    return hi;
}

/* ---- adopt bookkeeping (CDN-S3): a unit is runnable only if locally   */
/* authored (saved on this node) OR explicitly adopted by the operator.  */
#define SELFC_ADOPT_MAX 8
static char selfc_local[SELFC_ADOPT_MAX][PFS_NAME_MAX + 1];  /* saved here  */
static int  selfc_nlocal = 0;
static char selfc_adopted[SELFC_ADOPT_MAX][PFS_NAME_MAX + 1];/* `adopt`ed    */
static int  selfc_nadopted = 0;

static int name_in(char tbl[][PFS_NAME_MAX + 1], int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (strncmp(tbl[i], name, PFS_NAME_MAX) == 0) return 1;
    return 0;
}
static void selfc_note_local(const char *name)
{
    if (selfc_nlocal < SELFC_ADOPT_MAX && !name_in(selfc_local, selfc_nlocal, name)) {
        s_copy(selfc_local[selfc_nlocal], name, PFS_NAME_MAX + 1);
        selfc_nlocal++;
    }
}
static void selfc_note_adopt(const char *name)
{
    if (selfc_nadopted < SELFC_ADOPT_MAX && !name_in(selfc_adopted, selfc_nadopted, name)) {
        s_copy(selfc_adopted[selfc_nadopted], name, PFS_NAME_MAX + 1);
        selfc_nadopted++;
    }
}
static int selfc_is_accepted(const char *name)
{
    return name_in(selfc_local, selfc_nlocal, name)
        || name_in(selfc_adopted, selfc_nadopted, name);
}

/* `selfc run <name>` default path: germinate the head version in a germ
 * process (the crash boundary). Refuses non-local, non-adopted units (§3). */
static void selfc_run_germ(const char *name)
{
    if (!selfc_is_accepted(name)) {
        tm_printf((const UB *)"[selfc] REFUSING to run '%s' — not locally "
                  "authored and not adopted. selfc-ring3.md §3: v1 is "
                  "LOCAL-ONLY evolution (gossip replicates bytes; germination "
                  "is gated). Type `selfc adopt %s` to accept it explicitly.\n",
                  (const UB *)name, (const UB *)name);
        return;
    }
    U4 seq = selfc_head_seq(name);
    if (seq == 0) {
        tm_printf((const UB *)"[selfc] unit '%s' has no version in p-fs yet "
                  "(save it first, or it is still replicating)\n", (const UB *)name);
        return;
    }
    /* enqueue: the supervisor task compiles + forks (never the shell task). */
    selfc_germ_launch(name, seq);
}

static void selfc_ls(void)
{
    tm_printf((const UB *)"[selfc] %d unit(s) compiled this boot:\n",
              selfc_nunits);
    for (INT i = 0; i < selfc_nunits; i++)
        tm_printf((const UB *)"  #%d  task %d  %s\n",
                  i, (W)selfc_units[i].tid, (const UB *)selfc_units[i].what);
    tm_printf((const UB *)"[selfc] germ reaps this boot: %u (last termsig %d)\n",
              (unsigned)selfc_reaped_count, selfc_last_termsig);
}

/* ================================================================== */
/* the falsifiable acceptance gates (selfc-ring3.md §5)                */
/* ================================================================== */
/* The CURE phases run here in the germ-process default; the [selfc-isolated]
 * DISEASE phase (a crash unit KILLS the node) lives in the orchestrating
 * shell script (samples/11_distributed/run_selfc_isolation.sh, §5.1) because
 * a verb cannot survive its own process dying. Exact comparisons only. */

/* a unit that prints a token then null-derefs — the crash unit (§5.1). */
static const char selfc_crash_src[] =
    "int tm_printf(const unsigned char *fmt, ...);\n"
    "void selfc_main(void){\n"
    "  tm_printf((const unsigned char *)\"crash-unit alive: TOKEN=0xC0DE\\n\");\n"
    "  *(volatile int *)0 = 0;            /* SIGSEGV — the disease */\n"
    "}\n";

/* a unit that proves p-fs is NOT linkable (it names pfs_dag_save; relocate
 * must FAIL on the unresolved symbol — the link-time capability boundary). */
static const char selfc_pfs_src[] =
    "int pfs_dag_save(const unsigned char *n, unsigned w, const void *b, unsigned l);\n"
    "void selfc_main(void){ pfs_dag_save((const unsigned char *)\"x\",1,\"y\",1); }\n";

/* gate unit v1: GOOD — publishes a token on its own unit topic forever. */
static const char selfc_gate_v1_src[] =
    "int tm_printf(const unsigned char *fmt, ...);\n"
    "int tk_dly_tsk(unsigned int ms);\n"
    "int kdds_open(const char *name, int qos);\n"
    "int kdds_pub(int h, const void *data, int len);\n"
    "void selfc_main(void){\n"
    "  int h = kdds_open(\"unit/gate/out\", 0);\n"
    "  const char *tok = \"A:1\";\n"
    "  for(;;){ if(h>=0) kdds_pub(h, tok, 3);\n"
    "    tm_printf((const unsigned char *)\"gate v1 served A:1\\n\");\n"
    "    tk_dly_tsk(300); }\n"
    "}\n";

/* gate unit v2: BAD — crashes ~600ms after start (inside probation). */
static const char selfc_gate_v2_src[] =
    "int tm_printf(const unsigned char *fmt, ...);\n"
    "int tk_dly_tsk(unsigned int ms);\n"
    "void selfc_main(void){\n"
    "  tm_printf((const unsigned char *)\"gate v2 (bad build) starting\\n\");\n"
    "  tk_dly_tsk(600);\n"
    "  *(volatile int *)0 = 0;            /* dies in probation */\n"
    "}\n";

/* poll the supervisor until predicate or timeout (ms); yields to the
 * concurrent supervisor task via tk_dly_tsk. */
#define SELFC_T_POLL_MS  100

static void selfc_test(void)
{
    tm_printf((const UB *)"[selfc-test] ==== selfc-ring3 v1: the immune boundary "
              "(docs/architecture/selfc-ring3.md §5) ====\n");

    /* a sentinel: kdds round-trip proving the KERNEL is alive after a reap. */

    /* ---- capability boundary (link-time half, §1.2) ------------------- */
    {
        void *e = (void *)selfc_compile_resolve(selfc_pfs_src, "selfc_main",
                                                "pfs-probe", 1 /* isolated */);
        tm_printf((const UB *)"[selfc-test] capability(link): a unit naming "
                  "pfs_dag_save resolves entry=%s — p-fs is NOT in the API "
                  "table, so it cannot link (§1.2)\n", e ? "OK(BUG!)" : "NULL(refused)");
        if (e) tm_printf((const UB *)"[selfc-test] WARNING: p-fs linked — boundary breach\n");
    }

    /* ================= [selfc-isolated] (CURE) ====================== */
    {
        selfc_sup_reset();
        selfc_sup_set_probation(10000);          /* default-ish; crash is instant */
        U4 r0 = selfc_reaped_count;

        /* save + germinate the crash unit (the supervisor task compiles+forks
         * it; the cert never forks). */
        INT cs = pfs_dag_save((const UB *)"unit/crash", 10,
                              selfc_crash_src, (UW)s_len(selfc_crash_src));
        selfc_note_local("crash");
        INT clause_ok = (cs == PFS_OK);
        if (clause_ok) selfc_germ_launch("crash", 1);

        /* wait for exactly one reap (bounded), yielding to the supervisor task
         * that does the fork + reap. */
        for (INT t = 0; t < 120 && selfc_reaped_count == r0; t++)
            tk_dly_tsk(SELFC_T_POLL_MS);

        U4 reaped_delta = selfc_reaped_count - r0;

        /* sentinel: a fresh kdds round-trip AFTER the reap — kernel alive. */
        W sh = kdds_open_poll("selfc/sentinel", KDDS_QOS_LATEST_ONLY);
        INT sentinel_ok = 0;
        if (sh >= 0) {
            const char *ping = "PING";
            UB rb[8];
            if (kdds_pub(sh, ping, 4) == 0) {
                for (INT t = 0; t < 10; t++) {
                    W g = kdds_sub(sh, rb, sizeof rb, 0);
                    if (g == 4 && rb[0]=='P' && rb[1]=='I') { sentinel_ok = 1; break; }
                    tk_dly_tsk(50);
                }
            }
            kdds_close(sh);
        }

        INT pass = clause_ok
                && (reaped_delta == 1)
                && selfc_last_signaled
                && (selfc_last_termsig == SIGSEGV)
                && sentinel_ok;
        tm_printf((const UB *)"[selfc-test] isolated: reaped_delta=%u(==1) "
                  "signaled=%d termsig=%d(==%d SIGSEGV) sentinel=%d\n",
                  (unsigned)reaped_delta, selfc_last_signaled,
                  selfc_last_termsig, SIGSEGV, sentinel_ok);
        if (pass) tm_printf((const UB *)"[selfc-isolated] PASS\n");
        else      tm_printf((const UB *)"[selfc-isolated] FAIL\n");
    }

    /* ================= [selfc-rollback] ============================= */
    INT roll_pass = 0;
    {
        selfc_sup_reset();
        selfc_sup_set_probation(3000);           /* short probation for CI time */

        /* unit short name = "gate"; its SOURCE lives at p-fs ref "unit/gate"
         * and its topics under "unit/gate/" (§1.1). save v1 (good) then v2
         * (bad) — head becomes seq 2. */
        INT s1 = pfs_dag_save((const UB *)"unit/gate", 9,
                              selfc_gate_v1_src, (UW)s_len(selfc_gate_v1_src));
        INT s2 = pfs_dag_save((const UB *)"unit/gate", 9,
                              selfc_gate_v2_src, (UW)s_len(selfc_gate_v2_src));
        selfc_note_local("gate");

        U4 head = selfc_head_seq("gate");        /* should be 2 */
        /* germinate the head (v2): the supervisor task compiles + forks it. */
        selfc_germ_launch("gate", head);

        /* wait for v2 to die in probation and seq 1 to take over. */
        INT ran1 = 0;
        for (INT t = 0; t < 120; t++) {
            if (selfc_sup_running_seq("gate") == 1) { ran1 = 1; break; }
            tk_dly_tsk(SELFC_T_POLL_MS);
        }

        INT seq2_bad = selfc_sup_seq_is_bad("gate", 2);
        INT deaths   = selfc_sup_deaths("gate");

        /* served-token-after-rollback: subscribe unit/gate/out, see A:1 from
         * the v1 germ's OWN process (cannot be faked by a parent print). The
         * pump drains the v1 germ's PUB frames into the real kdds topic. */
        INT served = 0;
        W gh = kdds_open_poll("unit/gate/out", KDDS_QOS_LATEST_ONLY);
        if (gh >= 0) {
            UB rb[8];
            for (INT t = 0; t < 60; t++) {
                W g = kdds_sub(gh, rb, sizeof rb, 0);
                if (g == 3 && rb[0]=='A' && rb[1]==':' && rb[2]=='1') { served = 1; break; }
                tk_dly_tsk(SELFC_T_POLL_MS);
            }
            kdds_close(gh);
        }

        roll_pass = (s1 == PFS_OK) && (s2 == PFS_OK) && (head == 2)
                 && ran1 && seq2_bad && served && (deaths == 1);
        tm_printf((const UB *)"[selfc-test] rollback: head=%u(==2) ran_seq1=%d "
                  "seq2_bad=%d served_A:1=%d deaths=%d(==1)\n",
                  (unsigned)head, ran1, seq2_bad, served, deaths);
        if (roll_pass) tm_printf((const UB *)"[selfc-rollback] PASS\n");
        else           tm_printf((const UB *)"[selfc-rollback] FAIL\n");
    }

    /* reap the long-running v1 germ before the read-only lineage gate so no
     * germ child lingers across the (fork-free) lineage walk. */
    selfc_sup_reset();

    /* ================= [selfc-lineage] ============================= */
    {
        /* version chain walkable: read unit/gate @2 and @1; seqs contiguous,
         * content distinct. */
        static UB b1[PFS_BLOCK_MAX], b2[PFS_BLOCK_MAX];
        INT r2 = pfs_dag_read_at((const UB *)"unit/gate", 9, 2, b2, sizeof b2);
        INT r1 = pfs_dag_read_at((const UB *)"unit/gate", 9, 1, b1, sizeof b1);
        INT chain_walk = (r1 > 0) && (r2 > 0) && (r1 != r2 ||
                          memcmp(b1, b2, (size_t)(r1 < r2 ? r1 : r2)) != 0);

        /* self/lin chain hash-verifies AND carries the unit events in order:
         * at least germ@1, germ@2, reap@2, rollback (the rollback flow). */
        INT ng = 0, nr = 0, nrb = 0, okc = 0;
        INT verify = lm_self_unit_lineage_check(&ng, &nr, &nrb, &okc);
        INT events_ok = verify && okc && (ng >= 2) && (nr >= 1) && (nrb >= 1);

        tm_printf((const UB *)"[selfc-test] lineage: chain_walk=%d (v1 %d B, v2 "
                  "%d B, distinct) self/lin verify=%d germ=%d reap=%d rollback=%d\n",
                  chain_walk, r1, r2, okc, ng, nr, nrb);
        if (chain_walk && events_ok)
            tm_printf((const UB *)"[selfc-lineage] PASS\n");
        else
            tm_printf((const UB *)"[selfc-lineage] FAIL\n");
    }

    /* tidy up: ensure no germ lingers; restore the shipped probation. */
    selfc_sup_reset();
    selfc_sup_set_probation(10000);
}

/* DISEASE driver (selfc-ring3.md §5.1): compile the crash unit and run it
 * LEGACY in-kernel (no crash boundary). The fault re-executes and the whole
 * ./p-kernel process dies — the measurable disease the germ boundary cures.
 * Used by samples/11_distributed/run_selfc_isolation.sh. */
static void selfc_crashdemo(void)
{
    tm_printf((const UB *)"[selfc] crashdemo: running a null-deref unit LEGACY "
              "in-kernel (NO boundary) — this should KILL the node (§5.1 disease)\n");
    selfc_compile_and_run(selfc_crash_src, "selfc_main", "crashdemo");
    /* yield so the unguarded crash task gets the CPU and faults; if the
     * disease is real this never returns (the process dies). */
    for (INT i = 0; i < 20; i++) tk_dly_tsk(100);
    tm_printf((const UB *)"[selfc] crashdemo: node SURVIVED (unexpected — the "
              "unguarded fault did not kill it)\n");
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

/* no-libtcc stubs for the verbs selfc_cmd dispatches (the NO_LIBTCC stub
 * pattern, selfc.c:270 historic). selfc/ring3 is hosted+libtcc only; the
 * Play tier reaches these via SELFC_TIER_PLAY -> codegen off (§4). */
static int  selfc_isolation_on(void) { return 1; }
static void selfc_note_local(const char *name)  { (void)name; }
static void selfc_note_adopt(const char *name)
{
    (void)name;
    tm_printf((const UB *)"[selfc] no libtcc — adopt is a no-op (cannot "
              "germinate units without a compiler)\n");
}
static void selfc_run_germ(const char *name)
{
    (void)name;
    tm_printf((const UB *)"[selfc] this build has no libtcc — cannot "
              "germinate units. install libtcc-dev and rebuild.\n");
}
static void selfc_test(void)
{
    tm_printf((const UB *)"[selfc] no libtcc in this build — selfc-ring3 "
              "gates need a compiler. (native-only; see CI ump-x86_64)\n");
}
static void selfc_crashdemo(void)
{
    tm_printf((const UB *)"[selfc] no libtcc — cannot run the crash unit\n");
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
        "usage: selfc demo            compile+run the built-in demo (LEGACY in-task)\n"
        "       selfc save <name>     save demo C source to p-fs <name>\n"
        "       selfc adopt <name>    explicitly accept a unit for germination (§3)\n"
        "       selfc run <name>      germinate <name> in a germ process (crash boundary)\n"
        "       selfc test            run the selfc-ring3 acceptance gates (§5)\n"
        "       selfc ls              list compiled units + germ reaps\n");
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

    if (p < end && tok_is(p, end, "crashdemo")) {
        /* §5.1 disease driver: a crash unit run LEGACY in-kernel (kills node).*/
        selfc_crashdemo();

    } else if (p < end && tok_is(p, end, "demo")) {
        selfc_compile_and_run(selfc_demo_src, "selfc_main", "demo");

    } else if (p < end && tok_is(p, end, "save")) {
        p += 4;
        char nm[PFS_NAME_MAX + 1];
        INT  nl = selfc_token(&p, end, nm, sizeof nm);
        if (nl == 0) { selfc_usage(); return; }
        INT r = pfs_dag_save((const UB *)nm, (UW)nl,
                             selfc_demo_src, (UW)s_len(selfc_demo_src));
        if (r == PFS_OK) {
            selfc_note_local(nm);          /* locally authored -> runnable (§3) */
            tm_printf((const UB *)"[selfc] demo source (%d bytes of C) "
                      "saved as p-fs object '%s' — locally authored, "
                      "runnable; it now replicates like any block\n",
                      s_len(selfc_demo_src), (const UB *)nm);
        } else
            tm_printf((const UB *)"[selfc] pfs save failed (%d)\n", r);

    } else if (p < end && tok_is(p, end, "adopt")) {
        p += 5;
        char nm[PFS_NAME_MAX + 1];
        INT  nl = selfc_token(&p, end, nm, sizeof nm);
        if (nl == 0) { selfc_usage(); return; }
        selfc_note_adopt(nm);
        tm_printf((const UB *)"[selfc] ADOPTED '%s' — explicit operator accept "
                  "(selfc-ring3.md §3). It may now be germinated. (Isolation "
                  "is not trust: this is the LOCAL accept that stands in for "
                  "signatures, deferred to v3.)\n", (const UB *)nm);

    } else if (p < end && tok_is(p, end, "run")) {
        p += 3;
        char nm[PFS_NAME_MAX + 1];
        INT  nl = selfc_token(&p, end, nm, sizeof nm);
        if (nl == 0) { selfc_usage(); return; }
        if (!selfc_isolation_on()) {
            /* DISEASE path (SELFC_ISOLATE=0): legacy in-kernel task — a crash
             * kills the node. Kept exactly so [selfc-isolated] can capture the
             * real death (selfc-ring3.md §5.1). */
            INT len = -1;
            for (INT t = 0; t < 6; t++) {
                len = pfs_dag_read((const UB *)nm, (UW)nl,
                                   selfc_srcbuf, PFS_BLOCK_MAX);
                if (len >= 0) break;
                tk_dly_tsk(400);
            }
            if (len < 0) {
                tm_printf((const UB *)"[selfc] p-fs object '%s' not found (%d)\n",
                          (const UB *)nm, len);
                return;
            }
            selfc_srcbuf[len] = '\0';
            tm_printf((const UB *)"[selfc] SELFC_ISOLATE=0 — running '%s' "
                      "LEGACY in-kernel (no crash boundary)\n", (const UB *)nm);
            selfc_compile_and_run((const char *)selfc_srcbuf, "selfc_main", nm);
        } else {
            selfc_run_germ(nm);            /* v1 default: the crash boundary */
        }

    } else if (p < end && tok_is(p, end, "test")) {
        selfc_test();

    } else if (p < end && tok_is(p, end, "ls")) {
        selfc_ls();

    } else {
        selfc_usage();
    }
}
