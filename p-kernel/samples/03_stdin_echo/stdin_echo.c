/*
 *  07_stdin_echo/stdin_echo.c
 *
 *  stdin エコーサンプル
 *  SYS_READ(fd=0) で標準入力から 1 行読み込み、
 *  SYS_WRITE(fd=1) でそのまま標準出力へ返します。
 *  空行を入力すると終了します。
 *
 *  学べること:
 *    - sys_read(0, buf, len)  — 標準入力からの読み込み
 *    - sys_write(1, buf, len) — 標準出力への書き込み
 *    - sys_exit(code)         — プロセス終了
 *
 *  実行例 (QEMU, -serial stdio):
 *    p-kernel> exec stdin_echo.elf
 *    echo> hello
 *    hello
 *    echo> world
 *    world
 *    echo>          ← 空行で終了
 *    [proc] exited (code=0)
 */
#include "plibc.h"

/* 1 行読み込む (改行まで、または maxlen-1 バイトまで)
 * 戻り値: 読み込んだバイト数 (改行を含まない)              */
static int readline(char *buf, int maxlen)
{
    int n = 0;
    while (n < maxlen - 1) {
        char c;
        int r = sys_read(0, &c, 1);
        if (r <= 0) break;          /* EOF / エラー             */
        if (c == '\r') continue;    /* CR は無視               */
        if (c == '\n') break;       /* LF で行終端             */
        buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

void _start(void)
{
    plib_puts("========================================\r\n");
    plib_puts(" stdin_echo: 標準入力エコーデモ\r\n");
    plib_puts("========================================\r\n");
    plib_puts("  空行を入力すると終了します。\r\n\r\n");

    char line[128];

    for (;;) {
        /* プロンプトを表示 */
        plib_puts("echo> ");

        /* 1 行読み込む */
        int n = readline(line, sizeof(line));

        /* 空行 → 終了 */
        if (n == 0) {
            plib_puts("\r\n");
            break;
        }

        /* 入力した行をエコー */
        sys_write(1, line, n);
        plib_puts("\r\n");
    }

    plib_puts("========================================\r\n");
    plib_puts(" stdin_echo: done\r\n");
    plib_puts("========================================\r\n");

    sys_exit(0);
}
