/*
 *  persist.c (x86)
 *  Phase 7 — Flash/Disk Persistence for K-DDS Topics
 *
 *  kdds_topics[] の内容を FAT32 ディスクに定期保存し、
 *  起動時に ネットワーク復元よりも前に読み戻す。
 *
 *  ファイル命名規則:
 *    トピック名の '/' を '_' に置換し、先頭に "kd_"、末尾に ".dat" を付ける。
 *    例: "sensor/watchdog" → "/kd_sensor_watchdog.dat"
 *        "vital/0"         → "/kd_vital_0.dat"
 */

#include "persist.h"
#include "kdds.h"
#include "vfs.h"
#include "kernel.h"
#include <tmonitor.h>

/* ------------------------------------------------------------------ */
/* モジュール状態                                                      */
/* ------------------------------------------------------------------ */

PERSIST_STATS persist_stats;

/* ------------------------------------------------------------------ */
/* 内部ユーティリティ                                                  */
/* ------------------------------------------------------------------ */

static void ps_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    tm_putstring((UB *)s);
    (void)n;
}

static void ps_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { ps_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    ps_puts(&buf[i]);
}

/* トピック名 → ファイル名 変換
 *   "sensor/watchdog" → "/kd_sensor_watchdog.dat"
 *   out_buf: 少なくとも 48 bytes */
static void topic_to_filename(const char *name, char *out_buf)
{
    INT i = 0;
    out_buf[i++] = '/';
    out_buf[i++] = 'k';
    out_buf[i++] = 'd';
    out_buf[i++] = '_';
    for (INT j = 0; name[j] && i < 44; j++, i++)
        out_buf[i] = (name[j] == '/') ? '_' : name[j];
    out_buf[i++] = '.';
    out_buf[i++] = 'd';
    out_buf[i++] = 'a';
    out_buf[i++] = 't';
    out_buf[i]   = '\0';
}

static void ps_memcpy(void *dst, const void *src, INT n)
{
    const UB *s = (const UB *)src;
    UB *d = (UB *)dst;
    for (INT i = 0; i < n; i++) d[i] = s[i];
}

static INT ps_strlen(const char *s)
{
    INT n = 0; while (s[n]) n++; return n;
}

static INT ps_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

/* ------------------------------------------------------------------ */
/* persist_save_one — 単一トピックをディスクに書く                    */
/* ------------------------------------------------------------------ */

static void persist_save_one(const KDDS_TOPIC *t)
{
    PERSIST_RECORD rec;
    rec.magic    = PERSIST_MAGIC;
    rec.version  = PERSIST_VERSION;
    rec.qos      = t->qos;
    rec.data_len = t->data_len;

    /* name をゼロクリアしてコピー */
    for (INT i = 0; i < 32; i++) rec.name[i] = '\0';
    for (INT i = 0; t->name[i] && i < 31; i++) rec.name[i] = t->name[i];

    ps_memcpy(rec.data, t->data, t->data_len);
    /* 残りをゼロクリア */
    for (INT i = (INT)t->data_len; i < 128; i++) rec.data[i] = 0;

    char fname[48];
    topic_to_filename(t->name, fname);

    /* 既存ファイルを削除してから新規作成 (上書き相当) */
    vfs_unlink(fname);   /* エラーは無視 (ファイルが無い場合) */

    INT fd = vfs_create(fname);
    if (fd < 0) {
        ps_puts("[persist] create failed: ");
        ps_puts(fname);
        ps_puts("\r\n");
        persist_stats.errors++;
        return;
    }

    INT r = vfs_write(fd, &rec, (UW)sizeof(rec));
    vfs_close(fd);

    if (r < 0) {
        ps_puts("[persist] write failed: ");
        ps_puts(fname);
        ps_puts("\r\n");
        persist_stats.errors++;
        return;
    }

    persist_stats.saved++;
}

/* ------------------------------------------------------------------ */
/* persist_checkpoint — 全 open トピックを保存                        */
/* ------------------------------------------------------------------ */

void persist_checkpoint(void)
{
    if (!vfs_ready) return;

    INT count = 0;
    for (W i = 0; i < KDDS_TOPIC_MAX; i++) {
        if (!kdds_topics[i].open)     continue;
        if (kdds_topics[i].data_len == 0) continue;   /* 未発行トピックはスキップ */
        persist_save_one(&kdds_topics[i]);
        count++;
    }

    persist_stats.checkpoints++;
    if (count > 0) {
        ps_puts("[persist] checkpoint: ");
        ps_putdec((UW)count);
        ps_puts(" topic(s) saved\r\n");
    }
}

/* ------------------------------------------------------------------ */
/* persist_restore_all — 起動時にディスクから復元                     */
/* ------------------------------------------------------------------ */

void persist_restore_all(void)
{
    if (!vfs_ready) return;

    /* ルートディレクトリを列挙し "kd_*.dat" を探す */
    VFS_DIRENT entries[32];
    INT n = vfs_readdir("/", entries, 32);
    if (n < 0) return;

    INT restored = 0;

    for (INT e = 0; e < n; e++) {
        if (entries[e].is_dir) continue;

        /* "kd_" で始まり ".dat" で終わるファイルを対象とする */
        const char *nm = entries[e].name;
        INT len = ps_strlen(nm);
        if (len < 8) continue;   /* "kd_a.dat" = 8 chars minimum */
        if (nm[0] != 'k' || nm[1] != 'd' || nm[2] != '_') continue;
        if (nm[len-4] != '.' || nm[len-3] != 'd' ||
            nm[len-2] != 'a' || nm[len-1] != 't') continue;

        /* ファイルを読み込む */
        char fpath[48];
        fpath[0] = '/'; INT fi = 1;
        for (INT k = 0; nm[k] && fi < 47; k++) fpath[fi++] = nm[k];
        fpath[fi] = '\0';

        INT fd = vfs_open(fpath);
        if (fd < 0) continue;

        PERSIST_RECORD rec;
        INT r = vfs_read(fd, &rec, (UW)sizeof(rec));
        vfs_close(fd);

        if (r != (INT)sizeof(rec)) continue;
        if (rec.magic   != PERSIST_MAGIC)   continue;
        if (rec.version != PERSIST_VERSION) continue;
        if (rec.data_len == 0 || rec.data_len > 128) continue;

        /* kdds_topics[] に直接書き込む (ネットワーク前なので broadcast 不要) */
        W slot = -1;
        /* 既存トピックを探す */
        for (W i = 0; i < KDDS_TOPIC_MAX; i++) {
            if (kdds_topics[i].open && ps_streq(kdds_topics[i].name, rec.name)) {
                slot = i; break;
            }
        }
        /* なければ空きスロットを確保 */
        if (slot < 0) {
            for (W i = 0; i < KDDS_TOPIC_MAX; i++) {
                if (!kdds_topics[i].open) { slot = i; break; }
            }
        }
        if (slot < 0) {
            ps_puts("[persist] topic table full, cannot restore: ");
            ps_puts(rec.name);
            ps_puts("\r\n");
            continue;
        }

        /* スロットにデータを書き込む */
        KDDS_TOPIC *t = &kdds_topics[slot];
        for (INT i = 0; i < 32; i++) t->name[i] = rec.name[i];
        ps_memcpy(t->data, rec.data, rec.data_len);
        t->data_len = rec.data_len;
        t->qos      = rec.qos;
        t->data_seq = 1;   /* 復元済みを示す (0 = 未発行 と区別) */
        t->open     = 1;

        ps_puts("[persist] restored \"");
        ps_puts(rec.name);
        ps_puts("\"\r\n");

        restored++;
        persist_stats.restored++;
    }

    if (restored > 0) {
        ps_puts("[persist] total ");
        ps_putdec((UW)restored);
        ps_puts(" topic(s) restored from disk\r\n");
    }
}

/* ------------------------------------------------------------------ */
/* persist_task — 定期保存タスク                                      */
/* ------------------------------------------------------------------ */

void persist_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    ps_puts("[persist] task started (interval=");
    ps_putdec(PERSIST_INTERVAL_S);
    ps_puts("s)\r\n");

    /* 起動直後は少し待つ (他のタスクがトピックを開くまで) */
    tk_dly_tsk(5000);

    for (;;) {
        persist_checkpoint();
        tk_dly_tsk((TMO)(PERSIST_INTERVAL_S * 1000));
    }
}

/* ------------------------------------------------------------------ */
/* persist_list — ディスク上の保存済みトピックを表示                  */
/* ------------------------------------------------------------------ */

void persist_list(void)
{
    if (!vfs_ready) {
        ps_puts("[persist] disk not available\r\n");
        return;
    }

    VFS_DIRENT entries[32];
    INT n = vfs_readdir("/", entries, 32);
    if (n < 0) {
        ps_puts("[persist] readdir failed\r\n");
        return;
    }

    ps_puts("[persist] saved topics:\r\n");
    ps_puts("  File                              Name             Bytes\r\n");

    INT found = 0;
    for (INT e = 0; e < n; e++) {
        if (entries[e].is_dir) continue;
        const char *nm = entries[e].name;
        INT len = ps_strlen(nm);
        if (len < 8) continue;
        if (nm[0] != 'k' || nm[1] != 'd' || nm[2] != '_') continue;
        if (nm[len-4] != '.' || nm[len-3] != 'd' ||
            nm[len-2] != 'a' || nm[len-1] != 't') continue;

        /* レコードを読んでトピック名を取得 */
        char fpath[48];
        fpath[0] = '/'; INT fi = 1;
        for (INT k = 0; nm[k] && fi < 47; k++) fpath[fi++] = nm[k];
        fpath[fi] = '\0';

        INT fd = vfs_open(fpath);
        if (fd < 0) continue;

        PERSIST_RECORD rec;
        INT r = vfs_read(fd, &rec, (UW)sizeof(rec));
        vfs_close(fd);

        if (r != (INT)sizeof(rec) || rec.magic != PERSIST_MAGIC) continue;

        ps_puts("  "); ps_puts(nm);
        /* パディング */
        INT pad = 34 - ps_strlen(nm);
        for (INT p = 0; p < pad; p++) ps_puts(" ");
        ps_puts(rec.name);
        pad = 17 - ps_strlen(rec.name);
        for (INT p = 0; p < pad; p++) ps_puts(" ");
        ps_putdec(rec.data_len);
        ps_puts("\r\n");
        found++;
    }

    if (!found) ps_puts("  (なし)\r\n");

    ps_puts("[persist] stats: saved=");
    ps_putdec(persist_stats.saved);
    ps_puts(" restored=");
    ps_putdec(persist_stats.restored);
    ps_puts(" checkpoints=");
    ps_putdec(persist_stats.checkpoints);
    ps_puts(" errors=");
    ps_putdec(persist_stats.errors);
    ps_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* persist_clear — ディスク上の保存済みトピックを全削除               */
/* ------------------------------------------------------------------ */

void persist_clear(void)
{
    if (!vfs_ready) {
        ps_puts("[persist] disk not available\r\n");
        return;
    }

    VFS_DIRENT entries[32];
    INT n = vfs_readdir("/", entries, 32);
    if (n < 0) return;

    INT deleted = 0;
    for (INT e = 0; e < n; e++) {
        if (entries[e].is_dir) continue;
        const char *nm = entries[e].name;
        INT len = ps_strlen(nm);
        if (len < 8) continue;
        if (nm[0] != 'k' || nm[1] != 'd' || nm[2] != '_') continue;
        if (nm[len-4] != '.' || nm[len-3] != 'd' ||
            nm[len-2] != 'a' || nm[len-1] != 't') continue;

        char fpath[48];
        fpath[0] = '/'; INT fi = 1;
        for (INT k = 0; nm[k] && fi < 47; k++) fpath[fi++] = nm[k];
        fpath[fi] = '\0';

        if (vfs_unlink(fpath) == 0) {
            ps_puts("[persist] deleted: "); ps_puts(fpath); ps_puts("\r\n");
            deleted++;
        }
    }

    ps_puts("[persist] cleared ");
    ps_putdec((UW)deleted);
    ps_puts(" file(s)\r\n");
}
