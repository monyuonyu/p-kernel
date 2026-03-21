/*
 *  09_readdir/readdir.c
 *
 *  ディレクトリ一覧取得サンプル
 *  SYS_READDIR でディレクトリエントリを列挙します。
 *
 *  学べること:
 *    - sys_readdir(path, buf, max) — ディレクトリのエントリ一覧を取得する
 *    - PK_SYS_DIRENT 構造体 — name / size / is_dir フィールド
 *    - ファイルとディレクトリを判別して表示する
 *
 *  実行例:
 *    p-kernel> exec readdir.elf
 */
#include "plibc.h"

/* ファイルサイズを人間が読みやすい形式で出力 */
static void print_size(unsigned int size)
{
    if (size >= 1024 * 1024) {
        plib_putu(size / (1024 * 1024));
        plib_puts(" MB");
    } else if (size >= 1024) {
        plib_putu(size / 1024);
        plib_puts(" KB");
    } else {
        plib_putu(size);
        plib_puts("  B");
    }
}

/* 右寄せで最大 widthカラム幅で文字列を出力 (数値向け) */
static void print_rjust(unsigned int v, int width)
{
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    if (v == 0) { buf[--i] = '0'; }
    else { while (v > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; } }
    int len = 11 - i;
    while (len < width) { plib_puts(" "); len++; }
    plib_puts(buf + i);
}

/* ディレクトリを一覧表示する */
static void list_dir(const char *path)
{
    PK_SYS_DIRENT entries[32];

    int n = sys_readdir(path, entries, 32);
    if (n < 0) {
        plib_puts("  readdir(");
        plib_puts(path);
        plib_puts("): error\r\n");
        return;
    }
    if (n == 0) {
        plib_puts("  (empty)\r\n");
        return;
    }

    /* ヘッダ */
    plib_puts("  type  size       name\r\n");
    plib_puts("  ----  ---------  ");
    for (int i = 0; i < 40; i++) plib_puts("-");
    plib_puts("\r\n");

    int files = 0, dirs = 0;
    unsigned int total_bytes = 0;

    for (int i = 0; i < n; i++) {
        PK_SYS_DIRENT *e = &entries[i];

        if (e->is_dir) {
            plib_puts("  [dir] ");
            plib_puts("         ");   /* size 列は空欄 */
            dirs++;
        } else {
            plib_puts("  [fil] ");
            print_rjust(e->size, 7);
            plib_puts("  B");
            files++;
            total_bytes += e->size;
        }

        plib_puts("  ");
        plib_puts(e->name);
        plib_puts("\r\n");
    }

    /* サマリー */
    plib_puts("  ----  ---------  ");
    for (int i = 0; i < 40; i++) plib_puts("-");
    plib_puts("\r\n");
    plib_puts("  ");
    plib_puti(n);
    plib_puts(" entries (");
    plib_puti(files);
    plib_puts(" files, ");
    plib_puti(dirs);
    plib_puts(" dirs)   total: ");
    print_size(total_bytes);
    plib_puts("\r\n");
}

void _start(void)
{
    plib_puts("========================================\r\n");
    plib_puts(" readdir: ディレクトリ一覧デモ\r\n");
    plib_puts("========================================\r\n\r\n");

    /* ルートディレクトリを一覧表示 */
    plib_puts("--- / (root) ---\r\n");
    list_dir("/");

    plib_puts("\r\n========================================\r\n");
    plib_puts(" readdir: done\r\n");
    plib_puts("========================================\r\n");

    sys_exit(0);
}
