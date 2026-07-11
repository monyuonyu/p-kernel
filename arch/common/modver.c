/*
 *  modver.c — the fine-grained MODULE-VERSION REGISTRY (see modver.h).
 *
 *  ONE explicit static table, modver_table[], is the single source of truth.
 *  Every row's version is the module's OWN _VER / _VERSION constant, pulled
 *  from the module's OWN header (collected here, never re-declared) — so each
 *  module "carries its own version" and this TU merely gathers them into one
 *  queryable place. The galaxy /modules.json endpoint and any future engineer
 *  page read THIS table; there is no second list anywhere.
 *
 *  Adding a module is one line: include its header (so its _VER is in scope)
 *  and add a { "short-name", <MOD>_VER } row.
 *
 *  Honesty / scope:
 *    - The bare-metal / distributed subsystem versions are ALWAYS present
 *      (their headers are part of the arch/common include chain).
 *    - The host-side inference-engine modules (M1a–M1d gguf/quant/forward/
 *      tokenizer, NS-1 student) live under arch/common/llm/ and are HOST /
 *      Android-only ("身体" tier). Their headers pull in <stdint.h> and are
 *      not in the bare-metal include chain, so their rows are compiled in
 *      only under _TK_HOSTED_LIBC_. On a bare-metal build those modules are
 *      genuinely absent, so honestly they do not appear in the table there.
 *    - No allocation, no boot-time registration, no global constructor: the
 *      table is a compile-time array.
 */

#include "modver.h"

/* --- distributed / kernel subsystem versions: each from its OWN header --- */
#include "swim.h"          /* SWIM_VERSION       — membership/gossip wire    */
#include "kdds.h"          /* KDDS_VERSION       — pub/sub data distribution */
#include "drpc.h"          /* DRPC_VERSION       — node RPC / heartbeat wire */
#include "raft.h"          /* RAFT_VERSION       — consensus wire            */
#include "pmesh.h"         /* PMESH_VERSION      — partition mesh            */
#include "spawn.h"         /* SPAWN_VERSION      — distributed spawn wire    */
#include "kloader_task.h"  /* KLOAD_VERSION      — kernel module loader      */
#include "sfs.h"           /* SFS_VERSION        — simple FS                 */
#include "replica.h"       /* REPLICA_VERSION    — replication wire          */
#include "pfs_repl.h"      /* PFSR_VERSION       — PFS replication           */
#include "pfs_dag.h"       /* PFSD_VERSION       — PFS content-addressed DAG */
#include "retrieval.h"     /* RET_BLOB_VER       — retrieval blob format     */
#include "ark_profile.h"   /* ARK_PROF_VER       — ark profile / consent     */
#include "sign.h"          /* SIGN_MANIFEST_VER  — signed-manifest format    */
#include "genome.h"        /* GENOME_VER         — evolution genome blob     */
#include "lm_self.h"       /* LM_SELF_VER        — autobiographical self     */
#include "dtr.h"           /* DTR_WBLOB_VER, MT_WIRE_VER_VOCAB — mind weights*/

#ifdef _TK_HOSTED_LIBC_
/* host-side inference engine ("身体" tier) — present only on hosted builds. */
#include "llm/gguf.h"      /* GGUF_VER           — M1a GGUF reader           */
#include "llm/quant.h"     /* LLM_QUANT_VER      — M1b dequant-matmul        */
#include "llm/forward.h"   /* LLM_FORWARD_VER    — M1c Llama forward         */
#include "llm/tokenizer.h" /* LLM_TOKENIZER_VER  — M1d BPE tokenizer         */
#include "llm/student.h"   /* NS_STUDENT_VER     — NS-1 Cradle baby          */
#endif

/* ---------------------------------------------------------------------- */
/* the registry: one row per versioned module. FINE-GRAINED on purpose.    */
/* ---------------------------------------------------------------------- */

typedef struct {
    const char *name;     /* short, stable module key (page/JSON identifier) */
    int         version;  /* the module's OWN _VER / _VERSION constant        */
} modver_row;

static const modver_row modver_table[] = {
    /* registry self-version (the table format itself) */
    { "modver",        MODVER_VER          },

    /* --- distributed / mesh / consensus --- */
    { "swim",          SWIM_VERSION        },
    { "kdds",          KDDS_VERSION        },
    { "drpc",          DRPC_VERSION        },
    { "raft",          RAFT_VERSION        },
    { "pmesh",         PMESH_VERSION       },
    { "spawn",         SPAWN_VERSION       },
    { "kloader",       KLOAD_VERSION       },

    /* --- storage / filesystems / replication --- */
    { "sfs",           SFS_VERSION         },
    { "replica",       REPLICA_VERSION     },
    { "pfs-repl",      PFSR_VERSION        },
    { "pfs-dag",       PFSD_VERSION        },
    { "retrieval",     RET_BLOB_VER        },

    /* --- identity / provenance / evolution --- */
    { "ark-profile",   ARK_PROF_VER        },
    { "sign-manifest", SIGN_MANIFEST_VER   },
    { "genome",        GENOME_VER          },
    { "lm-self",       LM_SELF_VER         },

    /* --- the mind's weight + wire formats --- */
    { "dtr-wblob",     DTR_WBLOB_VER       },
    { "mind-wire",     MT_WIRE_VER_VOCAB   },

#ifdef _TK_HOSTED_LIBC_
    /* --- host-side inference engine (M1a–M1d, NS-1) --- */
    { "gguf",          GGUF_VER            },
    { "llm-quant",     LLM_QUANT_VER       },
    { "llm-forward",   LLM_FORWARD_VER     },
    { "llm-tokenizer", LLM_TOKENIZER_VER   },
    { "ns-student",    NS_STUDENT_VER      },
#endif
};

#define MODVER_N ((int)(sizeof(modver_table) / sizeof(modver_table[0])))

int modver_count(void)
{
    return MODVER_N;
}

const char *modver_name(int i)
{
    if (i < 0 || i >= MODVER_N) return "";
    return modver_table[i].name;
}

int modver_version(int i)
{
    if (i < 0 || i >= MODVER_N) return -1;
    return modver_table[i].version;
}

const char *modver_build_id(void)
{
#ifdef MODVER_BUILD
    return MODVER_BUILD;          /* build system supplied a real id          */
#else
    /* DETERMINISTIC honest fallback (was `__DATE__ " " __TIME__`).
     *
     * __TIME__ is a wall-clock HH:MM:SS string that changes every second. It
     * is emitted into .rodata (SHF_MERGE), and because merge-string placement
     * depends on the string bytes, a different second shifts a relocation-
     * resolved operand in the linked .text on ~1/6 of relinks of *identical*
     * source — breaking the ".text byte-identity" freeze (the crown gate) for
     * every target that links this TU (the hosted boot/linux* builds). modver
     * is the ONLY TU in the tree touching __TIME__/__DATE__, so dropping the
     * second-resolution wall-clock removes the sole source→.text nondeterminism.
     *
     * __DATE__ alone is stable across an entire build session (all objects in
     * one `make` see the same calendar day), and gcc derives __DATE__ from
     * SOURCE_DATE_EPOCH when that reproducible-builds env var is set — so a
     * pinned-epoch build is FULLY source-deterministic. Still honest: this is
     * the binary's real build date, never blank, and MODVER_BUILD (a git id)
     * overrides it whenever the build system supplies one. */
    return "unversioned " __DATE__;
#endif
}
