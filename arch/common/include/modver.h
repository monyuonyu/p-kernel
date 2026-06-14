/*
 *  modver.h — the fine-grained MODULE-VERSION REGISTRY.
 *
 *  mk_pino's directive: "each module carries a version, as fine-grained as
 *  possible", visible on the engineer page. This is the OBSERVABILITY side
 *  of the compatibility / evolution architecture
 *  (docs/architecture/compatibility.md): per-module versions are the
 *  migration chain made visible — you can SEE, at runtime, exactly which
 *  contract version each subsystem on this node speaks.
 *
 *  Design (single explicit table, one source of truth):
 *    arch/common/modver.c holds ONE static `modver_table[]` of
 *    {short-name, version} rows. Each row's version is the module's OWN
 *    `#define <MOD>_VER` / `<MOD>_VERSION` constant pulled from the module's
 *    OWN header — so the module carries its version and the registry merely
 *    COLLECTS it. Add a module = add its header's _VER define + one row.
 *
 *  No registration runs at boot, no allocation, no global ctor: the table is
 *  a compile-time array. modver_count()/_name()/_version() iterate it;
 *  modver_build_id() returns a build identifier (MODVER_BUILD if the build
 *  system defines one, else a compile-time __DATE__ " " __TIME__ string).
 *
 *  Plain C types only (int / const char*) so this header is includable from
 *  BOTH the bare-metal TUs and the host test harness without dragging in the
 *  T-Kernel type aliases.
 */
#ifndef PKERNEL_MODVER_H
#define PKERNEL_MODVER_H

/* modver.h carries its OWN version too: the registry's table-format / ABI.
 * v1 = {name,version} rows + build_id; /modules.json shape below. */
#define MODVER_VER  1

#ifdef __cplusplus
extern "C" {
#endif

/* Number of registered modules (>= 1). */
int          modver_count(void);

/* The i-th module's short name (e.g. "swim"), or "" if i is out of range.
 * The returned pointer is a static string literal — never freed. */
const char  *modver_name(int i);

/* The i-th module's version (>= 0), or -1 if i is out of range. */
int          modver_version(int i);

/* A build/commit identifier for this binary. Honest: if the build system
 * defines MODVER_BUILD it is used verbatim; otherwise it is the TU's
 * compile-time __DATE__ " " __TIME__. Static string — never freed. */
const char  *modver_build_id(void);

#ifdef __cplusplus
}
#endif

#endif /* PKERNEL_MODVER_H */
