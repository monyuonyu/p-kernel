/*
 *  13_mempool/mempool.c
 *
 *  Memory Pool (メモリプール) サンプル
 *  可変長プール (MPL) と固定長プール (MPF) の両方を実演します。
 *
 *  学べること:
 *    - tk_cre_mpl / tk_get_mpl / tk_rel_mpl / tk_del_mpl  — 可変長プール
 *    - tk_cre_mpf / tk_get_mpf / tk_rel_mpf / tk_del_mpf  — 固定長プール
 *    - ユーザーバッファを渡してカーネルがプールとして管理する仕組み
 *
 *  実行例:
 *    p-kernel> exec mempool.elf
 */
#include "plibc.h"

/* ===== 可変長プール ===== */
#define MPL_SIZE  512
static unsigned char mpl_buf[MPL_SIZE];

/* ===== 固定長プール ===== */
#define MPF_BLKSZ  32
#define MPF_COUNT  8
static unsigned char mpf_buf[MPF_BLKSZ * MPF_COUNT + 32]; /* +32: アライメント余裕 */

void _start(void)
{
    plib_puts("========================================\r\n");
    plib_puts(" mempool: メモリプールデモ\r\n");
    plib_puts("========================================\r\n\r\n");

    /* ---- 可変長プール ---- */
    plib_puts("--- Variable Memory Pool (MPL) ---\r\n");

    PK_CMPL cmpl = { 0, MPL_SIZE, mpl_buf };   /* mplatr=TA_TFIFO */
    int mplid = tk_cre_mpl(&cmpl);
    if (mplid < 0) {
        plib_puts("ERROR: tk_cre_mpl failed\r\n");
        sys_exit(1);
    }
    plib_puts("[+] mpl created (id="); plib_puti(mplid);
    plib_puts(" size="); plib_puti(MPL_SIZE); plib_puts(" bytes)\r\n");

    /* 3回アロケート → 全て解放 */
    int sizes[] = { 16, 48, 32 };
    void *ptrs[3] = { 0, 0, 0 };

    for (int i = 0; i < 3; i++) {
        ptrs[i] = tk_get_mpl(mplid, sizes[i], TMO_FEVR);
        plib_puts("  get["); plib_puti(i); plib_puts("]: sz=");
        plib_puti(sizes[i]);
        if (ptrs[i]) {
            plib_puts(" ptr=0x"); plib_putu((unsigned int)(long)ptrs[i]);
            plib_puts(" OK\r\n");
        } else {
            plib_puts(" FAILED\r\n");
        }
    }

    for (int i = 0; i < 3; i++) {
        if (ptrs[i]) {
            tk_rel_mpl(mplid, ptrs[i]);
            plib_puts("  rel["); plib_puti(i); plib_puts("]: OK\r\n");
        }
    }

    tk_del_mpl(mplid);
    plib_puts("[+] mpl deleted\r\n\r\n");

    /* ---- 固定長プール ---- */
    plib_puts("--- Fixed Memory Pool (MPF) ---\r\n");

    PK_CMPF cmpf = { 0, MPF_COUNT, MPF_BLKSZ, mpf_buf };  /* mpfatr=TA_TFIFO */
    int mpfid = tk_cre_mpf(&cmpf);
    if (mpfid < 0) {
        plib_puts("ERROR: tk_cre_mpf failed\r\n");
        sys_exit(1);
    }
    plib_puts("[+] mpf created (id="); plib_puti(mpfid);
    plib_puts(" blksz="); plib_puti(MPF_BLKSZ);
    plib_puts(" cnt=");   plib_puti(MPF_COUNT);
    plib_puts(")\r\n");

    /* 4ブロックアロケート */
    void *blks[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 4; i++) {
        blks[i] = tk_get_mpf(mpfid, TMO_FEVR);
        plib_puts("  get["); plib_puti(i); plib_puts("]: ");
        if (blks[i]) {
            plib_puts("ptr=0x"); plib_putu((unsigned int)(long)blks[i]);
            plib_puts(" OK\r\n");
        } else {
            plib_puts("FAILED\r\n");
        }
    }

    /* ポーリング取得 (ブロックがまだ残っている) */
    void *extra = tk_get_mpf(mpfid, TMO_POL);
    plib_puts("  get[poll]: ");
    if (extra) {
        plib_puts("ptr=0x"); plib_putu((unsigned int)(long)extra);
        plib_puts(" OK\r\n");
        tk_rel_mpf(mpfid, extra);
    } else {
        plib_puts("(none available)\r\n");
    }

    /* 全ブロック解放 */
    for (int i = 0; i < 4; i++) {
        if (blks[i]) {
            tk_rel_mpf(mpfid, blks[i]);
            plib_puts("  rel["); plib_puti(i); plib_puts("]: OK\r\n");
        }
    }

    tk_del_mpf(mpfid);
    plib_puts("[+] mpf deleted\r\n");

    plib_puts("\r\n========================================\r\n");
    plib_puts(" mempool: done\r\n");
    plib_puts("========================================\r\n");

    sys_exit(0);
}
