/*
 * arch/linux/test_node_id.c — N-0 cert: distinct, stable per-install ids.
 *
 *  Build (standalone, no kernel link needed):
 *    cc -O1 -I../../relay -o /tmp/test_node_id \
 *       test_node_id.c node_id.c ../../relay/sha256.c
 *    /tmp/test_node_id
 *
 *  Proves the four properties N-0 needs:
 *    1. two installs (two PFS dirs) derive DIFFERENT ids
 *    2. each id is STABLE across a second boot (re-read of the seed file)
 *    3. the legacy no-identity default is still 1 (single anonymous node)
 *    4. the PKERNEL_NODE_ID override is honoured verbatim (tests rely on it)
 *
 *  Exit 0 = PASS, non-zero = FAIL (one printf per clause).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

extern int pkernel_default_node_id(void);

static int derive_in_dir(const char *dir)
{
    setenv("PKERNEL_PFS_DIR", dir, 1);
    unsetenv("PKERNEL_NODE_ID");
    unsetenv("PKERNEL_NODE_SEED");
    return pkernel_default_node_id();
}

int main(void)
{
    char da[] = "/tmp/n0_inst_a_XXXXXX";
    char db[] = "/tmp/n0_inst_b_XXXXXX";
    if (!mkdtemp(da) || !mkdtemp(db)) { perror("mkdtemp"); return 2; }

    int a1 = derive_in_dir(da);
    int a2 = derive_in_dir(da);   /* second boot: must re-read the seed */
    int b1 = derive_in_dir(db);
    int b2 = derive_in_dir(db);

    int fail = 0;

    if (a1 >= 1 && a1 <= 63 && b1 >= 1 && b1 <= 63)
        printf("[node-id] T1 in-range: A=%d B=%d (1..63) — OK\n", a1, b1);
    else { printf("[node-id] T1 FAIL: out of range A=%d B=%d\n", a1, b1); fail = 1; }

    if (a1 == a2) printf("[node-id] T2 A stable across reboot (%d) — OK\n", a1);
    else { printf("[node-id] T2 FAIL: A unstable %d -> %d\n", a1, a2); fail = 1; }

    if (b1 == b2) printf("[node-id] T3 B stable across reboot (%d) — OK\n", b1);
    else { printf("[node-id] T3 FAIL: B unstable %d -> %d\n", b1, b2); fail = 1; }

    if (a1 != b1) printf("[node-id] T4 distinct: A=%d != B=%d — OK\n", a1, b1);
    else { printf("[node-id] T4 FAIL: collision A==B==%d\n", a1); fail = 1; }

    unsetenv("PKERNEL_PFS_DIR"); unsetenv("PKERNEL_NODE_ID"); unsetenv("PKERNEL_NODE_SEED");
    int leg = pkernel_default_node_id();
    if (leg == 1) printf("[node-id] T5 legacy no-identity default = 1 — OK\n");
    else { printf("[node-id] T5 FAIL: legacy default = %d (want 1)\n", leg); fail = 1; }

    setenv("PKERNEL_NODE_ID", "42", 1);
    int ov = pkernel_default_node_id();
    if (ov == 42) printf("[node-id] T6 override PKERNEL_NODE_ID=42 honoured — OK\n");
    else { printf("[node-id] T6 FAIL: override = %d (want 42)\n", ov); fail = 1; }

    /* cleanup */
    char buf[256];
    snprintf(buf, sizeof(buf), "%s/node_seed", da); unlink(buf); rmdir(da);
    snprintf(buf, sizeof(buf), "%s/node_seed", db); unlink(buf); rmdir(db);

    if (fail) { printf("[node-id] N-0 CERT FAIL\n"); return 1; }
    printf("[node-id] N-0 CERT PASS (6/6) — distinct + stable + override\n");
    return 0;
}
