/*
 *  05_posix2/posix2.c — 新 POSIX インターフェース デモ
 *
 *  テスト内容:
 *    getpid()   — タスク ID をプロセス ID として返す
 *    getcwd()   — カレントディレクトリ取得 (初期値 "/")
 *    chdir()    — カレントディレクトリ変更
 *    stat()     — パス指定でファイル情報取得
 *    fstat()    — fd 指定でファイル情報取得
 *    dup()      — ファイルディスクリプタの複製
 *    dup2()     — 指定番号へのファイルディスクリプタ複製
 *    pipe()     — プロセス内パイプ通信
 *
 *  実行方法:
 *    p-kernel> exec posix2.elf
 */
#include "plibc.h"

void _start(void)
{
    /* ---- getpid --------------------------------------------------- */
    plib_puts("[posix2] getpid = ");
    plib_puti(sys_getpid());
    plib_puts("\r\n");

    /* ---- getcwd --------------------------------------------------- */
    char cwd[128];
    sys_getcwd(cwd, sizeof(cwd));
    plib_puts("[posix2] getcwd = ");
    plib_puts(cwd);
    plib_puts("\r\n");

    /* ---- chdir ---------------------------------------------------- */
    sys_mkdir("/tmp");   /* なければ作成 */
    if (sys_chdir("/tmp") == 0) {
        char cwd2[128];
        sys_getcwd(cwd2, sizeof(cwd2));
        plib_puts("[posix2] chdir(/tmp) -> getcwd = ");
        plib_puts(cwd2);
        plib_puts("\r\n");
        sys_chdir("/");  /* ルートへ戻す */
    } else {
        plib_puts("[posix2] chdir(/tmp) failed\r\n");
    }

    /* ---- stat ----------------------------------------------------- */
    struct_stat st;
    if (sys_stat("/hello.elf", &st) == 0) {
        plib_puts("[posix2] stat(/hello.elf) size=");
        plib_puti((int)st.st_size);
        plib_puts(S_ISREG(st.st_mode) ? " type=REG\r\n" : " type=DIR\r\n");
    } else {
        plib_puts("[posix2] stat(/hello.elf) failed (no disk?)\r\n");
    }

    /* ---- fstat ---------------------------------------------------- */
    int fd = sys_open("/hello.elf", O_RDONLY);
    if (fd >= 0) {
        struct_stat fst;
        if (sys_fstat(fd, &fst) == 0) {
            plib_puts("[posix2] fstat fd=");
            plib_puti(fd);
            plib_puts(" size=");
            plib_puti((int)fst.st_size);
            plib_puts("\r\n");
        }

        /* ---- dup -------------------------------------------------- */
        int fd2 = sys_dup(fd);
        if (fd2 >= 0) {
            plib_puts("[posix2] dup  original_fd=");
            plib_puti(fd);
            plib_puts(" new_fd=");
            plib_puti(fd2);
            plib_puts("\r\n");
            sys_close(fd2);
        }

        /* ---- dup2 ------------------------------------------------- */
        /* fd を固定番号 9 に複製する (9 が空いていることを前提) */
        int target_fd = 9;
        int fd3 = sys_dup2(fd, target_fd);
        if (fd3 == target_fd) {
            plib_puts("[posix2] dup2 original_fd=");
            plib_puti(fd);
            plib_puts(" -> fixed_fd=");
            plib_puti(fd3);
            plib_puts("\r\n");
            sys_close(fd3);
        } else {
            plib_puts("[posix2] dup2 failed\r\n");
        }

        sys_close(fd);
    }

    /* ---- pipe ----------------------------------------------------- */
    int pfds[2];
    if (sys_pipe(pfds) == 0) {
        plib_puts("[posix2] pipe read_fd=");
        plib_puti(pfds[0]);
        plib_puts(" write_fd=");
        plib_puti(pfds[1]);
        plib_puts("\r\n");

        /* 書き込み端へデータを送信 */
        const char *msg = "Hello, pipe!";
        sys_write(pfds[1], msg, 12);
        sys_close(pfds[1]);   /* write end を閉じると read 側に EOF が通知される */

        /* 読み込み端からデータを受信 */
        char rbuf[32];
        int n = sys_read(pfds[0], rbuf, (int)sizeof(rbuf) - 1);
        if (n > 0) {
            rbuf[n] = '\0';
            plib_puts("[posix2] pipe recv: ");
            plib_puts(rbuf);
            plib_puts("\r\n");
        }
        sys_close(pfds[0]);
    } else {
        plib_puts("[posix2] pipe failed\r\n");
    }

    plib_puts("[posix2] done\r\n");
    sys_exit(0);
}
