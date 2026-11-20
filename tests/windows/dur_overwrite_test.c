/*
 * tests/windows/dur_overwrite_test.c — the hosted durable store's overwrite on
 * Windows (arch/linux/pfs_durable.c, linked as is). pfs_dur_write writes a
 * temp file and renames it over the name; msvcrt's rename() refuses when the
 * name is there, so every second write of a name (node.incarn, mind.rw,
 * ns1_student.bin, swarm-stop.st, ...) failed on Windows (read in
 * research/n2c-identity.md section 6, not measured until this test).
 *   D1 a first write of a new name reads back
 *   D2 a second write of the same name returns 0 and reads back the NEW bytes
 *   D3 no "<name>.tmp" is left behind
 * (pfs_dur_move_aside and node_ident.c's move_aside keep the plain rename on
 * purpose: they must not replace an existing target.)
 * CI: the windows-durable job (windows-latest). Negative -DPFS_DUR_NC_WIN_RENAME
 * (the plain rename) must fail D2.
 * Build (MinGW): x86_64-w64-mingw32-gcc -O1 -o dur_overwrite_test.exe \
 *   tests/windows/dur_overwrite_test.c arch/linux/pfs_durable.c
 * Run: set PKERNEL_PFS_DIR to an empty directory. Prints PASS/FAIL lines,
 * exit 0 when all pass.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

int pfs_dur_active(void);
int pfs_dur_write(const char *fname, const void *data, unsigned len);
int pfs_dur_read(const char *fname, void *buf, unsigned maxlen);

static int fails;
static void check(int ok, const char *id, const char *what)
{
    printf("  %s  [%s] %s\n", ok ? "PASS" : "FAIL", id, what);
    if (!ok) fails++;
}

int main(void)
{
    if (!pfs_dur_active()) { printf("  FAIL  [D0] PKERNEL_PFS_DIR is not set\n"); return 2; }
    char b[16];
    int w1 = pfs_dur_write("node.incarn", "\x01\x00\x00\x00", 4);
    int r1 = pfs_dur_read("node.incarn", b, sizeof b);
    check(w1 == 0 && r1 == 4 && b[0] == 1, "D1", "a first write reads back");
    int w2 = pfs_dur_write("node.incarn", "\x02\x00\x00\x00", 4);
    memset(b, 0, sizeof b);
    int r2 = pfs_dur_read("node.incarn", b, sizeof b);
    printf("  second write rc=%d, read %d bytes, first byte %d\n", w2, r2, (int)b[0]);
    check(w2 == 0 && r2 == 4 && b[0] == 2, "D2", "a second write of the same name returns 0 and reads back the new bytes");
    int w3 = pfs_dur_write("node.incarn", "\x03\x00\x00\x00", 4);
    memset(b, 0, sizeof b);
    int r3 = pfs_dur_read("node.incarn", b, sizeof b);
    check(w3 == 0 && r3 == 4 && b[0] == 3, "D2", "and a third");
    char tmp[600]; struct stat st;
    snprintf(tmp, sizeof tmp, "%s/node.incarn.tmp", getenv("PKERNEL_PFS_DIR"));
    check(stat(tmp, &st) != 0, "D3", "no node.incarn.tmp is left behind");
    printf("[dur-overwrite] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
