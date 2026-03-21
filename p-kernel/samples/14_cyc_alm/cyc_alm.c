/*
 *  14_cyc_alm/cyc_alm.c
 *
 *  Cyclic Handler / Alarm Handler サンプル
 *  周期ハンドラが一定間隔で実行カウンタをインクリメントし、
 *  アラームハンドラが指定時間後に1回だけ実行されます。
 *
 *  学べること:
 *    - tk_cre_cyc / tk_sta_cyc / tk_stp_cyc / tk_del_cyc  — 周期ハンドラ
 *    - tk_cre_alm / tk_sta_alm / tk_stp_alm / tk_del_alm  — アラームハンドラ
 *    - ハンドラはタスク独立文脈で実行 → ブロッキング呼び出し禁止
 *    - ハンドラからセマフォ sig / イベントフラグ set は可能
 *
 *  タイムライン:
 *    0ms  : cyclic 開始 (200ms周期), alarm 開始 (700ms後)
 *    200ms: cyc_handler 1回目
 *    400ms: cyc_handler 2回目
 *    600ms: cyc_handler 3回目
 *    700ms: alm_handler 実行
 *    800ms: main が結果チェック
 *
 *  実行例:
 *    p-kernel> exec cyc_alm.elf
 */
#include "plibc.h"

#define CYC_INTERVAL_MS  200
#define ALM_DELAY_MS     700
#define WAIT_MS          800

/* ハンドラからタスクに通知するセマフォ/フラグ */
static volatile int cyc_count = 0;
static volatile int alm_fired = 0;

/* 周期ハンドラ: ブロック禁止、グローバル変数のみ操作 */
static void cyc_handler(void)
{
    cyc_count++;
}

/* アラームハンドラ: 1回だけ実行 */
static void alm_handler(void)
{
    alm_fired = 1;
}

void _start(void)
{
    plib_puts("========================================\r\n");
    plib_puts(" cyc_alm: 周期/アラームハンドラデモ\r\n");
    plib_puts("========================================\r\n\r\n");

    /* 周期ハンドラ作成 (TA_CYC_STA: 作成と同時に開始) */
    PK_CCYC ccyc = {
        .cycatr    = TA_CYC_STA,
        .cychdr    = cyc_handler,
        .cyctim_ms = CYC_INTERVAL_MS,
        .cycphs_ms = 0,
    };
    int cycid = tk_cre_cyc(&ccyc);
    if (cycid < 0) {
        plib_puts("ERROR: tk_cre_cyc failed\r\n");
        sys_exit(1);
    }
    plib_puts("[+] cyclic handler created (id="); plib_puti(cycid);
    plib_puts(" interval="); plib_puti(CYC_INTERVAL_MS);
    plib_puts("ms)\r\n");

    /* アラームハンドラ作成 */
    PK_CALM calm = {
        .almatr = 0,
        .almhdr = alm_handler,
    };
    int almid = tk_cre_alm(&calm);
    if (almid < 0) {
        plib_puts("ERROR: tk_cre_alm failed\r\n");
        tk_del_cyc(cycid);
        sys_exit(1);
    }
    plib_puts("[+] alarm handler created  (id="); plib_puti(almid);
    plib_puts(" delay="); plib_puti(ALM_DELAY_MS);
    plib_puts("ms)\r\n");

    /* アラーム開始 */
    tk_sta_alm(almid, ALM_DELAY_MS);
    plib_puts("[+] alarm started\r\n");
    plib_puts("[+] waiting "); plib_puti(WAIT_MS); plib_puts("ms...\r\n\r\n");

    /* メインタスクが待機 */
    tk_slp_tsk(WAIT_MS);

    /* 周期ハンドラを停止 */
    tk_stp_cyc(cycid);

    /* 結果表示 */
    plib_puts("[result]\r\n");
    plib_puts("  cyc_count = "); plib_puti(cyc_count);
    plib_puts(" (expected ~"); plib_puti(WAIT_MS / CYC_INTERVAL_MS);
    plib_puts(")\r\n");

    plib_puts("  alm_fired = "); plib_puti(alm_fired);
    plib_puts(" (expected 1)\r\n");

    int cyc_ok = (cyc_count >= (WAIT_MS / CYC_INTERVAL_MS) - 1 &&
                  cyc_count <= (WAIT_MS / CYC_INTERVAL_MS) + 1);
    int alm_ok = (alm_fired == 1);

    plib_puts("  cyclic => "); plib_puts(cyc_ok ? "OK" : "NG"); plib_puts("\r\n");
    plib_puts("  alarm  => "); plib_puts(alm_ok ? "OK" : "NG"); plib_puts("\r\n");

    tk_del_cyc(cycid);
    tk_del_alm(almid);

    plib_puts("\r\n========================================\r\n");
    plib_puts(" cyc_alm: done\r\n");
    plib_puts("========================================\r\n");

    sys_exit(0);
}
