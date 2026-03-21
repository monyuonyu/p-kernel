/*
 *  test_all/test_posix.c — POSIX ファイル I/O テスト
 *
 *  テスト一覧:
 *    T1  — 基本ファイルI/O (open / write / read / close)
 *    T2  — lseek (SEEK_SET / SEEK_END)
 *    T3  — ディレクトリ・ファイル操作 (mkdir / rename / unlink)
 *    T20 — 部分読み (lseek 後に指定バイト数だけ読む)
 */

#include "test_common.h"

void test_posix(void)
{
    /* ----------------------------------------------------------------
     *  T1: POSIX open / write / read / close
     * ----------------------------------------------------------------
     *  ファイルを作成し、文字列を書き込んで閉じた後、
     *  読み込み専用で開き直して内容を検証します。
     */
    plib_puts("--- T1: POSIX 基本ファイルI/O ---\r\n");
    {
        /* O_WRONLY | O_CREAT でファイルを新規作成 */
        int fd = sys_open("/t1.txt", O_WRONLY | O_CREAT);
        ASSERT("T1-open", fd >= 3);   /* fd は必ず 3 以上 */
        if (fd >= 3) {
            int w = sys_write(fd, "HELLO", 5);
            ASSERT_EQ("T1-write", w, 5);   /* 5 バイト書けたか */
            sys_close(fd);

            /* 読み込み専用で開き直す */
            fd = sys_open("/t1.txt", O_RDONLY);
            ASSERT("T1-reopen", fd >= 3);
            char buf[8] = {0};
            int r = sys_read(fd, buf, 7);
            ASSERT_EQ("T1-read-len", r, 5);             /* 5 バイト読めたか */
            ASSERT("T1-read-data", buf[0]=='H' && buf[4]=='O');  /* 内容確認 */
            sys_close(fd);
        }
        sys_unlink("/t1.txt");   /* 後片付け */
    }

    /* ----------------------------------------------------------------
     *  T2: POSIX lseek — SEEK_SET / SEEK_END
     * ----------------------------------------------------------------
     *  SEEK_SET でファイル内の任意の位置に移動して読む。
     *  SEEK_END でファイル末尾に移動し、EOF を確認する。
     */
    plib_puts("\r\n--- T2: lseek SEEK_SET / SEEK_END ---\r\n");
    {
        /* "ABCDE" (5バイト) を書いたファイルを作成 */
        int fd = sys_open("/t2.txt", O_WRONLY | O_CREAT);
        sys_write(fd, "ABCDE", 5);
        sys_close(fd);

        fd = sys_open("/t2.txt", O_RDONLY);

        /* SEEK_SET(2): 先頭から 2 バイト目 ('C') に移動して 3 バイト読む */
        sys_lseek(fd, 2, SEEK_SET);
        char buf[4] = {0};
        int r = sys_read(fd, buf, 3);
        ASSERT_EQ("T2-lseek-set-len", r, 3);
        ASSERT("T2-lseek-set-data", buf[0]=='C' && buf[1]=='D' && buf[2]=='E');

        /* SEEK_END(0): 末尾に移動 → これ以上読めない (EOF = 0バイト) */
        sys_lseek(fd, 0, SEEK_END);
        r = sys_read(fd, buf, 3);
        ASSERT_EQ("T2-lseek-end", r, 0);   /* EOF なので 0 を期待 */
        sys_close(fd);
        sys_unlink("/t2.txt");
    }

    /* ----------------------------------------------------------------
     *  T3: POSIX mkdir / rename / unlink
     * ----------------------------------------------------------------
     *  ディレクトリを作成し、その中にファイルを作って rename する。
     *
     *  注意: p-kernel の fat32_unlink() はディレクトリ自体を削除できない。
     *        そのため、ディレクトリ内のファイルを先に削除してから
     *        プローブファイルでディレクトリの使用可能性を確認する。
     */
    plib_puts("\r\n--- T3: mkdir / rename / unlink ---\r\n");
    {
        /* 前回実行の残骸をクリーンアップ */
        sys_unlink("/testdir/f.txt");
        sys_unlink("/testdir/g.txt");
        sys_unlink("/testdir/.ok");

        /*
         *  mkdir: 新規作成なら 0、既存なら -1 を返す。
         *  どちらでもディレクトリは使える状態のはずなので、
         *  プローブファイルを作成できるかで確認する。
         */
        sys_mkdir("/testdir");
        int probe = sys_open("/testdir/.ok", O_WRONLY | O_CREAT);
        int r = (probe >= 3) ? 0 : -1;
        if (probe >= 3) { sys_close(probe); sys_unlink("/testdir/.ok"); }
        ASSERT("T3-mkdir", r == 0);

        /* ディレクトリ内にファイルを作成する */
        int fd = sys_open("/testdir/f.txt", O_WRONLY | O_CREAT);
        ASSERT("T3-open-in-dir", fd >= 3);
        if (fd >= 3) sys_close(fd);

        /* rename: f.txt → g.txt */
        r = sys_rename("/testdir/f.txt", "/testdir/g.txt");
        ASSERT_EQ("T3-rename", r, 0);

        /* 旧名 f.txt は存在しないはず */
        int fd2 = sys_open("/testdir/f.txt", O_RDONLY);
        ASSERT("T3-old-gone", fd2 < 0);

        /* 後片付け */
        sys_unlink("/testdir/g.txt");
    }

    /* ----------------------------------------------------------------
     *  T20: POSIX 部分読み (lseek 後に指定バイト数)
     * ----------------------------------------------------------------
     *  "0123456789" (10バイト) のファイルを作り、
     *  オフセット 5 から 4 バイトだけ読んで "5678" を確認する。
     */
    plib_puts("\r\n--- T20: POSIX 部分読み ---\r\n");
    {
        int fd = sys_open("/t20.txt", O_WRONLY | O_CREAT);
        sys_write(fd, "0123456789", 10);
        sys_close(fd);

        fd = sys_open("/t20.txt", O_RDONLY);
        char buf[12] = {0};

        /* オフセット 5 から 4 バイト読む → "5678" を期待 */
        sys_lseek(fd, 5, SEEK_SET);
        int r = sys_read(fd, buf, 4);
        ASSERT_EQ("T20-partial-len", r, 4);
        ASSERT("T20-partial-data", buf[0]=='5' && buf[3]=='8');
        sys_close(fd);
        sys_unlink("/t20.txt");
    }

    plib_puts("\r\n");
}
