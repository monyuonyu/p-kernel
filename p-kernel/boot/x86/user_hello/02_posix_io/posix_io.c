/*
 *  02_posix_io/posix_io.c — POSIX ファイル I/O サンプル
 *
 *  p-kernel は FAT32 ファイルシステムを内蔵しており、
 *  POSIX 互換の API でファイルを読み書きできます。
 *
 *  ─────────────────────────────────────────────
 *  ファイルディスクリプタ (fd) のルール
 *  ─────────────────────────────────────────────
 *
 *    fd = 0   標準入力  (現在未実装)
 *    fd = 1   標準出力  → シリアルポートに出力
 *    fd = 2   標準エラー→ シリアルポートに出力
 *    fd = 3〜 ファイル  → FAT32 ディスク上のファイル
 *
 *  sys_open() が返す fd は必ず 3 以上になります。
 *
 *  ─────────────────────────────────────────────
 *  このサンプルで学べること
 *  ─────────────────────────────────────────────
 *    1. ファイルの作成と書き込み (sys_open / sys_write)
 *    2. ファイルの読み込み       (sys_read)
 *    3. シーク操作               (sys_lseek)
 *    4. ディレクトリ作成         (sys_mkdir)
 *    5. ファイル名変更           (sys_rename)
 *    6. ファイル削除             (sys_unlink)
 *
 *  ビルド方法:
 *    make 02_posix_io/posix_io.elf
 *
 *  QEMU 実行方法:
 *    p-kernel> exec posix_io.elf
 */

#include "plibc.h"

/* ─────────────────────────────────────────────────────────────
 *  デモ関数のプロトタイプ
 * ───────────────────────────────────────────────────────────── */
static void demo_write_read(void);
static void demo_seek(void);
static void demo_directory(void);

/* ─────────────────────────────────────────────────────────────
 *  エントリーポイント
 * ───────────────────────────────────────────────────────────── */
void _start(void)
{
    plib_puts("=== POSIX ファイル I/O サンプル ===\r\n\r\n");

    demo_write_read();
    demo_seek();
    demo_directory();

    plib_puts("\r\n=== 完了 ===\r\n");
    plib_puts("次のサンプル: exec rtos_task.elf\r\n");
    sys_exit(0);
}

/* ─────────────────────────────────────────────────────────────
 *  1. ファイルの書き込みと読み込み
 * ───────────────────────────────────────────────────────────── */
static void demo_write_read(void)
{
    plib_puts("--- [1] ファイル書き込み / 読み込み ---\r\n");

    /* ── ファイルを開く (作成モード) ────────────────────────────
     *
     * sys_open(path, flags) でファイルを開きます。
     *   O_WRONLY | O_CREAT  : 書き込み専用 + ファイルが無ければ作成
     *   O_RDONLY            : 読み込み専用
     *
     * 成功すると fd (3 以上の整数) が返ります。
     * 失敗すると -1 が返ります。
     */
    int fd = sys_open("/sample.txt", O_WRONLY | O_CREAT);
    if (fd < 0) {
        plib_puts("  [ERROR] ファイルオープン失敗\r\n");
        return;
    }
    plib_puts("  ファイル作成: /sample.txt (fd=");
    plib_puti(fd);
    plib_puts(")\r\n");

    /* ── 文字列を書き込む ────────────────────────────────────── */
    const char *text = "p-kernel から書き込みました。\n";
    int written = sys_write(fd, text, plib_strlen(text));
    plib_puts("  書き込みバイト数: ");
    plib_puti(written);
    plib_puts("\r\n");

    /* ── ファイルを閉じる ────────────────────────────────────── */
    sys_close(fd);
    plib_puts("  ファイルをクローズしました\r\n");

    /* ── 同じファイルを読み込み専用で開く ────────────────────── */
    fd = sys_open("/sample.txt", O_RDONLY);
    if (fd < 0) {
        plib_puts("  [ERROR] 読み込みオープン失敗\r\n");
        return;
    }

    /* ── 読み込む ────────────────────────────────────────────── */
    char buf[64];
    int n = sys_read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';                       /* null 終端 */
        plib_puts("  読み込み内容: \"");
        sys_write(1, buf, n);               /* そのまま出力 */
        plib_puts("\"\r\n");
    }
    sys_close(fd);

    /* ── ファイルを削除して後片付け ──────────────────────────── */
    sys_unlink("/sample.txt");
    plib_puts("  /sample.txt を削除しました\r\n\r\n");
}

/* ─────────────────────────────────────────────────────────────
 *  2. シーク操作（ファイル内の位置移動）
 * ───────────────────────────────────────────────────────────── */
static void demo_seek(void)
{
    plib_puts("--- [2] lseek によるシーク操作 ---\r\n");

    /* まず 10 バイトのテストファイルを作成 */
    int fd = sys_open("/seek.txt", O_WRONLY | O_CREAT);
    sys_write(fd, "ABCDEFGHIJ", 10);   /* 10 文字書き込み */
    sys_close(fd);

    fd = sys_open("/seek.txt", O_RDONLY);

    /* ── SEEK_SET: ファイル先頭からのオフセット ─────────────── */
    sys_lseek(fd, 3, SEEK_SET);        /* 先頭から 3 バイト目に移動 */
    char buf[4] = {0};
    sys_read(fd, buf, 3);              /* 3 バイト読む → "DEF" */
    plib_puts("  SEEK_SET(3) から 3 バイト読み: \"");
    sys_write(1, buf, 3);
    plib_puts("\"\r\n");

    /* ── SEEK_END: ファイル末尾への移動 ─────────────────────── */
    sys_lseek(fd, 0, SEEK_END);        /* ファイル末尾に移動 */
    int r = sys_read(fd, buf, 1);      /* EOF なので 0 バイト返る */
    plib_puts("  SEEK_END 後の read: ");
    plib_puti(r);
    plib_puts(" バイト (0 = EOF)\r\n");

    sys_close(fd);
    sys_unlink("/seek.txt");
    plib_puts("\r\n");
}

/* ─────────────────────────────────────────────────────────────
 *  3. ディレクトリとファイル名変更
 * ───────────────────────────────────────────────────────────── */
static void demo_directory(void)
{
    plib_puts("--- [3] ディレクトリ作成 / ファイル名変更 ---\r\n");

    /* 前回実行の残骸をクリーンアップ */
    sys_unlink("/mydir/old.txt");
    sys_unlink("/mydir/new.txt");

    /* ── ディレクトリ作成 ────────────────────────────────────── */
    int r = sys_mkdir("/mydir");
    if (r == 0)
        plib_puts("  /mydir を作成しました\r\n");
    else
        plib_puts("  /mydir は既に存在します (再利用)\r\n");

    /* ── ディレクトリ内にファイルを作成 ──────────────────────── */
    int fd = sys_open("/mydir/old.txt", O_WRONLY | O_CREAT);
    sys_write(fd, "Hello", 5);
    sys_close(fd);
    plib_puts("  /mydir/old.txt を作成しました\r\n");

    /* ── ファイル名変更 ──────────────────────────────────────── */
    r = sys_rename("/mydir/old.txt", "/mydir/new.txt");
    if (r == 0)
        plib_puts("  old.txt → new.txt にリネームしました\r\n");

    /* 旧名でオープン試行（失敗するはず） */
    int fd_old = sys_open("/mydir/old.txt", O_RDONLY);
    if (fd_old < 0)
        plib_puts("  /mydir/old.txt は存在しません (正常)\r\n");

    /* 新名でオープン（成功するはず） */
    int fd_new = sys_open("/mydir/new.txt", O_RDONLY);
    if (fd_new >= 0) {
        plib_puts("  /mydir/new.txt は存在します (正常)\r\n");
        sys_close(fd_new);
    }

    /* クリーンアップ */
    sys_unlink("/mydir/new.txt");
    plib_puts("  クリーンアップ完了\r\n\r\n");
}
