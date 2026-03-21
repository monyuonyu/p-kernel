/*
 *  edf_infer.c — SLA 付き推論デモ (Phase 4)
 *
 *  sys_infer_sla(packed, deadline_ms) でカーネルに推論を依頼する。
 *  カーネルはノード負荷と deadline を見て自動的に最適ノードへルーティングする。
 *
 *  2 ノード構成 (make run-node0 / run-node1) で実行すると、
 *  node0 のパイプラインが高負荷のとき node1 へ自動オフロードされる。
 *
 *  実行後に `edf stat` シェルコマンドでオフロード統計を確認できる。
 */

#include "plibc.h"

static void puts_s(const char *s) { sys_write(1, s, plib_strlen(s)); }

static void putu(unsigned v)
{
    char buf[12]; int i = 11; buf[i] = '\0';
    if (!v) { puts_s("0"); return; }
    while (v && i > 0) { buf[--i] = '0' + (char)(v % 10); v /= 10; }
    puts_s(&buf[i]);
}

static const char *cls_name(int c)
{
    if (c == 0) return "normal  ";
    if (c == 1) return "alert   ";
    if (c == 2) return "critical";
    return "error   ";
}

/* センサーデータのバリエーション (int8 正規化済み値) */
static const int SENSORS[5][4] = {
    {   0,   0,   0,   0 },   /* normal   : 常温・標準              */
    {  40,  30,  20,  60 },   /* alert    : 高温・多湿              */
    {  80,  60,  40,  80 },   /* critical : 超高温・極端値          */
    {  10,  -5,   5,  20 },   /* normal   : 若干低温                */
    {  60,  50,  35,  70 },   /* alert    : 高温気味                */
};

void _start(void)
{
    puts_s("[edf_infer] SLA 付き推論デモ開始\r\n");
    puts_s("[edf_infer] deadline=2ms (tight) と 50ms (relaxed) で比較\r\n\r\n");
    puts_s("  idx  deadline  class     packed\r\n");
    puts_s("  ---  --------  --------  ------\r\n");

    for (int i = 0; i < 10; i++) {
        int s      = i % 5;
        int packed = SYS_SENSOR_PACK(SENSORS[s][0], SENSORS[s][1],
                                     SENSORS[s][2], SENSORS[s][3]);

        /* タイトな締め切り: 2ms — 高負荷時は他ノードへオフロード */
        int cls = sys_infer_sla(packed, 2);
        puts_s("  "); putu((unsigned)i);
        puts_s("    2ms       "); puts_s(cls_name(cls));
        puts_s("\r\n");

        /* 余裕のある締め切り: 50ms — 常にローカル実行 */
        cls = sys_infer_sla(packed, 50);
        puts_s("  "); putu((unsigned)i);
        puts_s("    50ms      "); puts_s(cls_name(cls));
        puts_s("\r\n");

        tk_slp_tsk(300);
    }

    puts_s("\r\n[edf_infer] done  (`edf stat` で統計確認)\r\n");
    sys_exit(0);
}
