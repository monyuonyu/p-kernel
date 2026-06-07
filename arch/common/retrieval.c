/*
 *  retrieval.c — Wave 8 ①: forward パスが p-fs の記憶を読む。
 *
 *  仕組みの全体像は retrieval.h 冒頭のコメント参照。要点:
 *
 *    - `dtr remember` が訓練セットの代表 32 サンプル (埋め込み+ラベル)
 *      を p-fs named ref "dtr/engrams" に 1 ブロックで保存する。
 *    - 推論/評価の forward は softmax 直前に ret_blend() を呼ぶ。
 *      ON のとき engram を p-fs からロードし (初回のみ、以後キャッシュ)、
 *      入力埋め込みとの L2 距離 top-k の票を logits に加算する。
 *    - p-fs に "dtr/engrams" が無ければ票はゼロ — 記憶が源。キャッシュは
 *      あくまで高速化で、出所は常に p-fs (ret_drop で読み直せる)。
 *
 *  honest limits: 玩具データセット (4ch int8 センサ、3 クラス)、engram
 *  は 4KB 1 ブロック上限 (32 個 × 20 B + 16 B ヘッダ = 656 B)、類似度は
 *  生入力空間の L2 — 学習された表現ではない。それでも「思考中に記憶を
 *  参照しなければ出ない精度」が測定できる (run_memory_thought.sh)。
 */

#include "retrieval.h"
#include "pfs_block.h"
#include "pfs_dag.h"
#include "kernel.h"
#include <tmonitor.h>

/* ------------------------------------------------------------------ */
/* 出力ヘルパー                                                        */
/* ------------------------------------------------------------------ */

static void rt_puts(const char *s) { tm_putstring((UB *)s); }

static void rt_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { rt_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    rt_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* 永続イメージ — static (スタック規律) + packed、レイアウト固定       */
/* ------------------------------------------------------------------ */

static struct __attribute__((packed, aligned(4))) {
    RET_BLOB_HDR h;                 /* 16 B -> e[] は 4-aligned         */
    RET_ENGRAM   e[RET_ENGRAM_N];
} rblob;

_Static_assert(sizeof(float) == 4, "float must be IEEE754 binary32");
_Static_assert(sizeof(RET_BLOB_HDR) == 16, "engram blob header is 16 B");
_Static_assert(sizeof(RET_ENGRAM) == 20, "engram entry is 20 B");
_Static_assert(sizeof(rblob) == 16 + RET_ENGRAM_N * 20,
               "engram blob must be header + 32 packed entries");
_Static_assert(sizeof(rblob) <= PFS_BLOCK_MAX,
               "engram blob must fit one p-fs block");

static UB ret_on_flag = 0;          /* ブレンド ON/OFF (default OFF)    */
static UB ret_loaded  = 0;          /* rblob がキャッシュ済みか         */

/* ------------------------------------------------------------------ */
/* 埋め込み — 重み非依存 (理由は retrieval.h 参照)                     */
/* ------------------------------------------------------------------ */

void ret_embed(const B input[DTR_SEQ_LEN], float e[RET_DIM])
{
    for (INT t = 0; t < RET_DIM; t++)
        e[t] = (float)input[t] / 127.0f;
}

/* ------------------------------------------------------------------ */
/* remember — 代表サンプルを engram として p-fs に保存                 */
/* ------------------------------------------------------------------ */

/* クラス均等 (32 = 11+11+10) かつクラス内等間隔に選ぶ。決定的:
 * 同じデータセットならどのノードでも同じ engram ブロック = 同じ
 * content-id になる。 */
INT ret_remember(const B (*X)[DTR_SEQ_LEN], const UB *y, UW n)
{
    if (n == 0) return PFS_E_INVAL;

    UW cnt = 0;
    UW percls = RET_ENGRAM_N / DTR_OUT_DIM;
    UW extra  = RET_ENGRAM_N - percls * DTR_OUT_DIM;

    for (UB c = 0; c < DTR_OUT_DIM; c++) {
        UW want = percls + (c < extra ? 1 : 0);

        UW nc = 0;                          /* クラス c の出現数        */
        for (UW i = 0; i < n; i++) if (y[i] == c) nc++;
        if (nc == 0) continue;
        if (want > nc) want = nc;

        for (UW j = 0; j < want; j++) {
            UW target = j * nc / want;      /* クラス内等間隔           */
            UW seen = 0;
            for (UW i = 0; i < n; i++) {
                if (y[i] != c) continue;
                if (seen == target) {
                    /* packed メンバへの直接ポインタは取らない
                     * (-Waddress-of-packed-member): 一旦ローカルへ */
                    float emb[RET_DIM];
                    ret_embed(X[i], emb);
                    for (INT d = 0; d < RET_DIM; d++)
                        rblob.e[cnt].e[d] = emb[d];
                    rblob.e[cnt].label   = c;
                    rblob.e[cnt]._pad[0] = 0;
                    rblob.e[cnt]._pad[1] = 0;
                    rblob.e[cnt]._pad[2] = 0;
                    cnt++;
                    break;
                }
                seen++;
            }
        }
    }

    /* 未使用スロットはゼロ詰め (content-id を決定的に保つ) */
    for (UW i = cnt; i < RET_ENGRAM_N; i++) {
        for (INT d = 0; d < RET_DIM; d++) rblob.e[i].e[d] = 0.0f;
        rblob.e[i].label = 0;
        rblob.e[i]._pad[0] = rblob.e[i]._pad[1] = rblob.e[i]._pad[2] = 0;
    }

    rblob.h.magic   = RET_BLOB_MAGIC;
    rblob.h.version = RET_BLOB_VER;
    rblob.h.count   = cnt;
    rblob.h.dim     = RET_DIM;
    rblob.h.out_dim = DTR_OUT_DIM;
    rblob.h._pad[0] = rblob.h._pad[1] = 0;

    INT r = pfs_dag_save((const UB *)RET_REF, RET_REF_LEN,
                         &rblob, (UW)sizeof(rblob));
    if (r != PFS_OK) {
        ret_loaded = 0;
        rt_puts("[ret] remember failed (pfs err ");
        rt_putdec((UW)(-r)); rt_puts(")\r\n");
        return r;
    }
    ret_loaded = 1;                  /* 保存した本人はキャッシュ有効     */

    rt_puts("[ret] "); rt_putdec(cnt);
    rt_puts(" engrams ("); rt_putdec((UW)sizeof(rblob));
    rt_puts(" B) saved as p-fs object '" RET_REF
            "' — the swarm can now think with this memory\r\n");
    return PFS_OK;
}

/* ------------------------------------------------------------------ */
/* load — p-fs が唯一の出所                                            */
/* ------------------------------------------------------------------ */

static UB blob_valid(INT got)
{
    return (got == (INT)sizeof(rblob) &&
            rblob.h.magic   == RET_BLOB_MAGIC &&
            rblob.h.version == RET_BLOB_VER   &&
            rblob.h.count   >= 1              &&
            rblob.h.count   <= RET_ENGRAM_N   &&
            rblob.h.dim     == RET_DIM        &&
            rblob.h.out_dim == DTR_OUT_DIM);
}

UW ret_avail(void)
{
    if (ret_loaded) return rblob.h.count;

    INT r = pfs_dag_read((const UB *)RET_REF, RET_REF_LEN,
                         &rblob, (UW)sizeof(rblob));
    if (r == PFS_E_NOTFOUND) return 0;   /* want は発行済み — 後で再試行 */
    if (!blob_valid(r)) return 0;

    ret_loaded = 1;
    rt_puts("[ret] engrams loaded from p-fs '" RET_REF "' (");
    rt_putdec(rblob.h.count); rt_puts(" entries)\r\n");
    return rblob.h.count;
}

void ret_drop(void) { ret_loaded = 0; }

UB ret_set(UB on)
{
    UB prev = ret_on_flag;
    ret_on_flag = on ? 1 : 0;
    return prev;
}

UB ret_get(void) { return ret_on_flag; }

/* ------------------------------------------------------------------ */
/* blend — softmax 直前の logits に記憶の票を加算                      */
/* ------------------------------------------------------------------ */

INT ret_blend(const B input[DTR_SEQ_LEN], float logits[DTR_OUT_DIM])
{
    if (!ret_on_flag) return 0;
    if (ret_avail() == 0) return 0;      /* p-fs に記憶が無い → 票ゼロ  */

    /* confidence gate: 票の重みを (1 - p_max)^2 でスケールする。
     * 重みが確信している (学習済み・p_max≈0.95) ときは記憶は囁く程度、
     * 重みが迷っている (未学習・p_max≈0.4) ときは記憶が決める。
     * 測定で選んだ設計 (3条件の精度で直接比較した結果):
     *   - フラット α=2.0      : (b) 93.3% だが (c) held-out 100%→95%
     *                           — 確信して正答していた重みを反転させた
     *   - 線形 gate (1-pmax)  : (c) 100%→96.7%、まだ 2/60 反転
     *   - 二乗 gate (1-pmax)^2: (b) 93.3% のまま (c) 96.3%/100% —
     *                           学習済み精度を壊さず train +1.3pt
     * top-k unanimity (近傍が割れたら棄権) も試したが (b) 93.3%→73.3%
     * と記憶だけで考える力を大きく削るため不採用。 */
    float p[DTR_OUT_DIM], pmax_sum = 0.0f, pmax;
    {
        float mx = logits[0];
        for (INT c = 1; c < DTR_OUT_DIM; c++)
            if (logits[c] > mx) mx = logits[c];
        for (INT c = 0; c < DTR_OUT_DIM; c++) {
            p[c] = dtr_expf(logits[c] - mx);
            pmax_sum += p[c];
        }
        pmax = p[0];
        for (INT c = 1; c < DTR_OUT_DIM; c++) if (p[c] > pmax) pmax = p[c];
        pmax /= (pmax_sum > 1e-10f ? pmax_sum : 1e-10f);
    }
    float gate = 1.0f - pmax;            /* in [0, 1-1/DOUT]            */
    gate = gate * gate;                  /* squared — 上の測定ノート参照 */

    float q[RET_DIM];
    ret_embed(input, q);

    /* top-k (挿入ソート、k は小さい) */
    INT   idx[RET_TOPK];
    float dd [RET_TOPK];
    for (INT k = 0; k < RET_TOPK; k++) { idx[k] = -1; dd[k] = 1e30f; }

    UW cnt = rblob.h.count;
    for (UW i = 0; i < cnt; i++) {
        float d2 = 0.0f;
        for (INT d = 0; d < RET_DIM; d++) {
            float df = q[d] - rblob.e[i].e[d];
            d2 += df * df;
        }
        for (INT k = 0; k < RET_TOPK; k++) {
            if (d2 < dd[k]) {
                for (INT m = RET_TOPK - 1; m > k; m--) {
                    dd[m] = dd[m - 1]; idx[m] = idx[m - 1];
                }
                dd[k] = d2; idx[k] = (INT)i;
                break;
            }
        }
    }

    INT voted = 0;
    for (INT k = 0; k < RET_TOPK; k++) {
        if (idx[k] < 0) continue;
        UB lab = rblob.e[idx[k]].label;
        if (lab >= DTR_OUT_DIM) continue;
        float sim = 1.0f / (1.0f + dd[k]);
        logits[lab] += RET_ALPHA * gate * sim;
        voted++;
    }
    return voted;
}

/* ------------------------------------------------------------------ */
/* stat                                                                */
/* ------------------------------------------------------------------ */

void ret_stat(void)
{
    rt_puts("[ret] retrieval: ");
    rt_puts(ret_on_flag ? "ON" : "OFF");
    rt_puts("  engrams: ");
    if (ret_loaded) {
        rt_putdec(rblob.h.count);
        rt_puts(" cached (source: p-fs '" RET_REF "')");
    } else {
        rt_puts("not loaded (p-fs '" RET_REF "' is the only source)");
    }
    rt_puts("  k="); rt_putdec(RET_TOPK);
    rt_puts(" alpha=2.0 gate=(1-pmax)^2\r\n");
}
