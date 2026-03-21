/*
 *  05_all_demo/flg.c — イベントフラグ デモ
 *
 *  デモ一覧:
 *    D7 — TWF_ANDW: 指定した全ビットがセットされるまで待つ
 *    D8 — TWF_ORW:  指定したいずれかのビットがセットされるまで待つ
 *    D9 — TMO_POL + tk_clr_flg: ポーリングとクリア操作
 */

#include "common.h"

void demo_flg(void)
{
    /* ----------------------------------------------------------------
     *  T7: イベントフラグ TWF_ANDW (AND 待ち)
     * ----------------------------------------------------------------
     *  bit0 と bit1 の「両方がセット」されるまで待つパターンです。
     *
     *  タスクA が bit0 (0x01) をセット
     *  タスクB が bit1 (0x02) をセット
     *  メインは 0x03 の TWF_ANDW で待つ → 両ビットが揃ったら通過
     *
     *  「複数の条件が全部成立するまで待つ」という AND 同期に使います。
     */
    plib_puts("--- T7: イベントフラグ TWF_ANDW ---\r\n");
    {
        PK_CFLG cflg = { 0, 0x00 };     /* exinf=0, 初期パターン=0x00 */
        g_flg = tk_cre_flg(&cflg);
        ASSERT("T7-cre-flg", g_flg >= 0);

        /*
         *  exinf のエンコード: (フラグID << 16) | ビットパターン
         *  task_flg_set がこれをデコードして tk_set_flg を呼ぶ
         */
        int packed_a = (g_flg << 16) | 0x01;
        int packed_b = (g_flg << 16) | 0x02;
        int t7a = start_task(task_flg_set, 8, SCHED_FIFO, 0,
                             0, (void*)(long)packed_a);
        int t7b = start_task(task_flg_set, 8, SCHED_FIFO, 0,
                             0, (void*)(long)packed_b);

        unsigned int ptn = 0;
        /* 0x03 の全ビット (bit0, bit1) がセットされるまで AND 待ち */
        int r = tk_wai_flg(g_flg, 0x03, TWF_ANDW, &ptn, 5000);
        ASSERT_EQ("T7-andw-ret", r, 0);
        ASSERT_EQ("T7-andw-ptn", (int)ptn, 0x03);   /* 0x03 が成立 */
        tk_del_tsk(t7a); tk_del_tsk(t7b);
        tk_del_flg(g_flg);
    }

    /* ----------------------------------------------------------------
     *  T8: イベントフラグ TWF_ORW (OR 待ち)
     * ----------------------------------------------------------------
     *  bit0 または bit2 の「どちらか一方でもセット」されれば通過するパターンです。
     *
     *  タスクが bit2 (0x04) のみをセット
     *  メインは 0x05 (bit0|bit2) の TWF_ORW で待つ
     *  → bit2 だけでも条件成立 (OR なので)
     *
     *  「複数の条件のどれかが成立すれば進む」という OR 同期に使います。
     */
    plib_puts("\r\n--- T8: イベントフラグ TWF_ORW ---\r\n");
    {
        PK_CFLG cflg = { 0, 0x00 };
        g_flg = tk_cre_flg(&cflg);

        int packed = (g_flg << 16) | 0x04;   /* bit2 だけセット */
        int t8a = start_task(task_flg_set, 8, SCHED_FIFO, 0,
                             0, (void*)(long)packed);

        unsigned int ptn = 0;
        /* bit0 OR bit2 で待つ → bit2 だけセットされても通過 */
        int r = tk_wai_flg(g_flg, 0x05, TWF_ORW, &ptn, 5000);
        ASSERT_EQ("T8-orw-ret", r, 0);
        ASSERT("T8-orw-ptn", (ptn & 0x04) != 0);   /* bit2 がセットされている */
        tk_del_tsk(t8a);
        tk_del_flg(g_flg);
    }

    /* ----------------------------------------------------------------
     *  T9: イベントフラグ TMO_POL + tk_clr_flg
     * ----------------------------------------------------------------
     *  ポーリングモードとクリア操作を確認します。
     *
     *  ① パターン 0x00 → TMO_POL 失敗 (E_TMOUT = -50)
     *  ② tk_set_flg(0xFF) → パターン 0xFF にセット
     *     TMO_POL 成功 (bit0 が含まれているので 0 が返る)
     *  ③ tk_clr_flg で全クリア → TMO_POL 再び失敗
     *
     *  tk_clr_flg の引数は「残すビットのマスク」です。
     *  全クリアしたい場合は 0x00 を渡します。
     */
    plib_puts("\r\n--- T9: イベントフラグ TMO_POL + clr ---\r\n");
    {
        PK_CFLG cflg = { 0, 0x00 };
        g_flg = tk_cre_flg(&cflg);

        /* ① パターン 0 → ポーリング失敗 */
        unsigned int ptn = 0;
        int r = tk_wai_flg(g_flg, 0x01, TWF_ANDW, &ptn, TMO_POL);
        ASSERT_EQ("T9-pol-empty", r, -50);

        /* ② 全ビットセット → ポーリング成功 */
        tk_set_flg(g_flg, 0xFF);
        r = tk_wai_flg(g_flg, 0x01, TWF_ANDW, &ptn, TMO_POL);
        ASSERT_EQ("T9-pol-hit", r, 0);

        /* ③ 全クリア → ポーリング再び失敗 */
        tk_clr_flg(g_flg, ~0x01u);   /* bit0 以外をクリア (bit0 は残す) */
        tk_clr_flg(g_flg, 0x00);     /* bit0 も含め全クリア */
        r = tk_wai_flg(g_flg, 0x01, TWF_ANDW, &ptn, TMO_POL);
        ASSERT_EQ("T9-after-clr", r, -50);
        tk_del_flg(g_flg);
    }

    plib_puts("\r\n");
}
