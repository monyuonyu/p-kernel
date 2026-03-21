/*
 *  edf.c (x86)
 *  リアルタイム AI スケジューリング — フェーズ 4
 *
 *  ノード選択ポリシー:
 *    deadline_ms <  5: ローカル負荷 > 10% → 最軽量ノードへオフロード
 *    deadline_ms < 20: ローカル負荷 > 50% → 最軽量ノードへオフロード
 *    deadline_ms >= 20: 常にローカル実行 (余裕あり)
 *
 *  負荷スコア (0-100):
 *    pipeline_count() × 100 / PIPELINE_DEPTH
 *    500ms ごとに K-DDS "L/N" トピックへ発行し、クラスタ全体で共有。
 */

#include "edf.h"
#include "kdds.h"
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

static void ed_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void ed_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { ed_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    ed_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* モジュール状態                                                      */
/* ------------------------------------------------------------------ */

EDF_STATS edf_stats;
UB        peer_load[DNODE_MAX];

/* 負荷タスクが動的に開くサブスクライバハンドル (ノードごと) */
static W sub_h[DNODE_MAX];

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void edf_init(void)
{
    for (INT i = 0; i < DNODE_MAX; i++) {
        peer_load[i] = 0;
        sub_h[i]     = -1;
    }
    edf_stats.total     = 0;
    edf_stats.local     = 0;
    edf_stats.offloaded = 0;
    edf_stats.sla_hit   = 0;
    edf_stats.sla_miss  = 0;
    ed_puts("[edf] リアルタイム AI スケジューラ ready\r\n");
}

/* ------------------------------------------------------------------ */
/* ローカル負荷スコア (0-100)                                         */
/* ------------------------------------------------------------------ */

UB edf_local_load(void)
{
    UW q     = pipeline_count();
    UW score = (q * 100) / PIPELINE_DEPTH;
    return (UB)(score > 100 ? 100 : score);
}

/* ------------------------------------------------------------------ */
/* ノード選択                                                          */
/* ------------------------------------------------------------------ */

static UB pick_best_node(W deadline_ms)
{
    if (drpc_my_node == 0xFF) return 0xFF;  /* 単体モードはローカル固定 */

    UB local = peer_load[drpc_my_node];

    /* deadline に応じたしきい値でオフロード判断 */
    UB threshold;
    if      (deadline_ms < 5)  threshold = EDF_STRICT_THR;
    else if (deadline_ms < 20) threshold = EDF_NORMAL_THR;
    else                       return drpc_my_node;  /* 余裕あり */

    if (local <= threshold) return drpc_my_node;

    /* 最も負荷の低い ALIVE ノードを探す */
    UB best      = drpc_my_node;
    UB best_load = local;
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == drpc_my_node) continue;
        if (dnode_table[n].state != DNODE_ALIVE) continue;
        if (peer_load[n] < best_load) {
            best_load = peer_load[n];
            best      = n;
        }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* SLA 付き推論                                                        */
/* ------------------------------------------------------------------ */

W edf_infer(W sensor_packed, W deadline_ms)
{
    edf_stats.total++;

    /* 開始時刻 */
    SYSTIM t0;
    tk_get_otm(&t0);

    UB best = pick_best_node(deadline_ms);
    UB cls  = 0;

    if (best == drpc_my_node || best == 0xFF) {
        /* ローカル推論 */
        B input[MLP_IN] = {
            SENSOR_UNPACK_T(sensor_packed),
            SENSOR_UNPACK_H(sensor_packed),
            SENSOR_UNPACK_P(sensor_packed),
            SENSOR_UNPACK_L(sensor_packed),
        };
        cls = mlp_forward(input);
        edf_stats.local++;
    } else {
        /* リモートへオフロード */
        ed_puts("[edf] offload → node "); ed_putdec(best);
        ed_puts(" (local="); ed_putdec(peer_load[drpc_my_node]);
        ed_puts("% dl="); ed_putdec((UW)deadline_ms); ed_puts("ms)\r\n");

        ER er = dtk_infer(best, sensor_packed, &cls, 3000);
        if (er != E_OK) {
            /* フォールバック: ローカルで実行 */
            B input[MLP_IN] = {
                SENSOR_UNPACK_T(sensor_packed),
                SENSOR_UNPACK_H(sensor_packed),
                SENSOR_UNPACK_P(sensor_packed),
                SENSOR_UNPACK_L(sensor_packed),
            };
            cls = mlp_forward(input);
            edf_stats.local++;
        } else {
            edf_stats.offloaded++;
        }
    }

    /* SLA 評価 */
    SYSTIM t1;
    tk_get_otm(&t1);
    UW elapsed = t1.lo - t0.lo;  /* ms 差分 */

    if ((W)elapsed <= deadline_ms) {
        edf_stats.sla_hit++;
    } else {
        edf_stats.sla_miss++;
        ed_puts("[edf] SLA miss  elapsed="); ed_putdec(elapsed);
        ed_puts("ms  deadline="); ed_putdec((UW)deadline_ms); ed_puts("ms\r\n");
    }

    return (W)cls;
}

/* ------------------------------------------------------------------ */
/* 負荷タスク: 発行 + ピア受信を 500ms ごとに実行                    */
/* ------------------------------------------------------------------ */

void edf_load_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    /* 自分の負荷を発行するパブリッシャを開く */
    char my_topic[4] = {
        'L', '/',
        (char)('0' + drpc_my_node),
        '\0'
    };
    W pub_h = kdds_open(my_topic, KDDS_QOS_LATEST_ONLY);

    for (;;) {
        tk_dly_tsk(500);
        if (drpc_my_node == 0xFF) continue;

        /* 新たに ALIVE になったピアのサブスクライバを動的に開く */
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (n == drpc_my_node || sub_h[n] >= 0) continue;
            if (dnode_table[n].state == DNODE_ALIVE) {
                char tn[4] = { 'L', '/', (char)('0' + n), '\0' };
                sub_h[n] = kdds_open(tn, KDDS_QOS_LATEST_ONLY);
                if (sub_h[n] >= 0) {
                    ed_puts("[edf] subscribed to load/"); ed_putdec(n); ed_puts("\r\n");
                }
            }
        }

        /* 自分の負荷を更新して発行 */
        UB load_score = edf_local_load();
        peer_load[drpc_my_node] = load_score;
        if (pub_h >= 0) kdds_pub(pub_h, &load_score, 1);

        /* ピアの負荷をポーリング受信 */
        for (UB n = 0; n < DNODE_MAX; n++) {
            if (sub_h[n] < 0) continue;
            UB score;
            W r = kdds_sub(sub_h[n], &score, 1, 0);  /* ポーリング */
            if (r == 1) peer_load[n] = score;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 統計表示                                                            */
/* ------------------------------------------------------------------ */

void edf_stat(void)
{
    ed_puts("[edf] ===== AI スケジューリング統計 =====\r\n");
    ed_puts("  合計推論    : "); ed_putdec(edf_stats.total);     ed_puts("\r\n");
    ed_puts("  ローカル    : "); ed_putdec(edf_stats.local);     ed_puts("\r\n");
    ed_puts("  オフロード  : "); ed_putdec(edf_stats.offloaded); ed_puts("\r\n");
    ed_puts("  SLA 達成    : "); ed_putdec(edf_stats.sla_hit);   ed_puts("\r\n");
    ed_puts("  SLA 違反    : "); ed_putdec(edf_stats.sla_miss);  ed_puts("\r\n");
    if (drpc_my_node != 0xFF) {
        ed_puts("[edf] ===== ノード負荷 (0-100%) =====\r\n");
        for (UB n = 0; n < DNODE_MAX; n++) {
            UB st = dnode_table[n].state;
            if (n == drpc_my_node) {
                ed_puts("  node "); ed_putdec(n);
                ed_puts(" (self) load="); ed_putdec(peer_load[n]); ed_puts("%\r\n");
            } else if (st == DNODE_ALIVE || st == DNODE_SUSPECT) {
                ed_puts("  node "); ed_putdec(n);
                ed_puts("        load="); ed_putdec(peer_load[n]); ed_puts("%\r\n");
            }
        }
    }
}
