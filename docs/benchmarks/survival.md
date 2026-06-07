# 生存ベンチマーク — 「K 台殺しても生きて、賢さが保たれる」を数字にする

測定日: 2026-06-06
ハーネス: [`samples/11_distributed/run_survival_bench.sh`](../../samples/11_distributed/run_survival_bench.sh)
対象: [`docs/assessment_structural_gaps.md`](../assessment_structural_gaps.md) 穴⑥（測定機構）の残り半分 —
「*more nodes = smarter / fewer nodes = more resilient* の **resilient 側を誰も測れない**」という指摘に対する実測データ。

## 何を測るか

実プロセスの実クラスタで、3 フェーズを連続実行し全アサーションの PASS/FAIL と所要秒を記録する。

| フェーズ | 内容 | アサーション |
|---|---|---|
| 1 学習と伝播 | node1 が `dtr train`（実 SGD・解析的 backprop）→ `dtr save`（重み → p-fs 版管理オブジェクト `dtr/weights`）。他の全ノードは **学習せず** p-fs P1 チャンク複製 + P2 ref gossip で blob を受領し `dtr load` | 全 N ノードが held-out 学習済み精度（>=90%）で eval / 全ノードプロセス生存 |
| 2 虐殺と生存 | K ノードを **SIGKILL**（N=4→K=1, N>=8→K=3）→ SWIM 収束待ち | 生存プロセス全数生存（巻き添えクラッシュ検出）/ `nodes`・`world` が N-K live + K DEAD を表示 / 全生存ノードが学習済み精度のまま / `pfs ls` に 2560 B 重み blob 残存 / kdds region pub fanout == 生存数-1 / 分散 `infer` がクラスを返す |
| 3 帰還 | 殺した 1 台を再起動（同じ node id・新プロセス＝未学習の新個体）| 生存ノードが ALIVE 復帰を観測 / 復帰直後は未学習（~26.7%）/ p-fs gossip で重み再取得 / `dtr eval` が学習済み精度 — **新しい個体が群れの記憶を受け継ぐ** |

- 実行系: aarch64 Linux (Termux proot) 上の `so_node` プロセス群（APK と同一の `libpkernel.so`）+ v2 relay 1 本。
- 駆動方式: 各ノードの stdin を FIFO 化し、ログ出力の観測に同期してコマンド投入（盲目的 sleep に依存しない）。
- いずれかのアサーションが落ちると exit 非 0。チェリーピックなし: 学習前のベースライン精度も同じ `dtr eval` が出力する。

## 実測結果

精度はすべて held-out split（60 サンプル）の accuracy。未学習ベースラインは 26.7%（chance ~33%、決定論的 LCG 初期化）。

### N=4 / K=1（連続 2 回 full PASS、22 アサーション/回）

1 回目（run E）:

| phase | live nodes | held-out acc (min) | data survival | elapsed (s) |
|---|---|---|---|---|
| 1 learn + propagate | 4/4 | 100.0% (all 4 nodes) | dtr/weights on 4/4 nodes | 8 |
| 2 kill 1 (SIGKILL) | 3/4 | 100.0% (all survivors) | blob intact on 3/3 survivors | 28 (SWIM 22) |
| 3 node4 returns | 4/4 | 26.7% → 100.0% (revived) | re-fetched via p-fs gossip | 14 (rejoin 6) |

2 回目（run F、直後に連続実行）:

| phase | live nodes | held-out acc (min) | data survival | elapsed (s) |
|---|---|---|---|---|
| 1 learn + propagate | 4/4 | 100.0% (all 4 nodes) | dtr/weights on 4/4 nodes | 7 |
| 2 kill 1 (SIGKILL) | 3/4 | 100.0% (all survivors) | blob intact on 3/3 survivors | 27 (SWIM 22) |
| 3 node4 returns | 4/4 | 26.7% → 100.0% (revived) | re-fetched via p-fs gossip | 8 (rejoin 6) |

kdds region pub fanout（殺害後）: 2 peers = 生存数-1（期待値どおり）。

### N=8 / K=3（確証ラン 1 回、28 アサーション、開始時空きメモリ 3.9 GB）

| phase | live nodes | held-out acc (min) | data survival | elapsed (s) |
|---|---|---|---|---|
| 1 learn + propagate | 8/8 | 100.0% (all 8 nodes) | dtr/weights on 8/8 nodes | 9 |
| 2 kill 3 (SIGKILL) | 5/8 | 100.0% (all survivors) | blob intact on 5/5 survivors | 39 (SWIM 35) |
| 3 node8 returns | 6/8 | 26.7% → 100.0% (revived) | re-fetched via p-fs gossip | 6 (rejoin 6) |

kdds region pub fanout（殺害後）: 4 peers = 生存数-1（期待値どおり）。28/28 PASS。

## 解釈（事実のみ）

- クラスタの 37.5%（3/8）を SIGKILL しても、生存ノードの精度は劣化しない（重みは各ノードのメモリ + p-fs ローカル複製に存在するため、ピア死は推論精度に影響しない）。劣化するのは膜の方 — SWIM の死亡確定に K=1 で 14〜22 秒（4 ラン実測）、K=3 で 35 秒（SUSPECT 2 ラウンド + DEAD 3 ラウンド + 噂伝播、1 秒プローブ周期の設計値どおり）。
- データ生存は複製係数の話で、N=4/K=1・N=8/K=3 の構成では全生存ノードが blob を保持していた（region 内全複製）。
- 帰還ノードの「未学習 26.7% → 6 秒で mesh 復帰 → p-fs gossip 経由で再学習なしに 100.0%」は両 N で再現。学習コストを払うのはクラスタで 1 回だけ、という性質が落ちても保たれる。

## 再現方法

```sh
cd samples/11_distributed
N=4 ./run_survival_bench.sh     # 開発デフォルト
N=8 ./run_survival_bench.sh     # K=3 自動選択
```

## 測定中に見つかり修正された問題

ベンチ自身が bring-up 中に本物のバグを 1 件検出した（このベンチの存在意義の傍証として記録する）:

- **SIGALRM tick のスレッド誤配送**（commit `edc4408`）: signal-as-IRQ タイマが `SIGEV_SIGNAL`（プロセス宛）だったため、`so_node` のようにカーネルを pthread で走らせるランチャでは tick がランチャ main スレッドに配送されることがあり、`knl_timer_handler_startup` がカーネルスレッドのタスクコードと**並走**して ready/timer キューを破壊していた。症状は (a) 隣接ノード死亡直後の生存ノード segfault、(b) 2 ノードが同一 PC（`knl_ready_queue_delete` 配下の `knl_tstdlib_bitsearch1`）で 99% CPU の livelock。`SIGEV_THREAD_ID` でカーネルスレッドに固定して修正。並列負荷が高いほど発火しやすく、単発スモークでは出ない種類の競合だった。

## 既知の限界

- localhost 1 ホスト上の N プロセス。実ネットワーク遅延・パーティションは relay の zone シミュレーション（`ZONE_SIZE`）を使っておらず未測定。
- 「賢さ」の指標は 635 パラメータ Transformer の held-out 精度 1 本。タスクが小さく、精度が落ちる前にノードが死に絶える側（K/N を上げた劣化カーブ）の測定は未着手。
- Phase 3 の再学習なし復元は p-fs 複製が生存ノードに残っていることが前提。全滅（K=N）からの復元は対象外。
- 観測済みの未解決フレーク 1 件: 再起動直後のノードの shell が stdin 1 行を取りこぼす事象が N=4 の計測中に 2 回（4 ラン中）発生した（1 回は 1 行のみ・自然回復、1 回は 45 秒以上継続しランが FAIL）。ハーネス側は `dtr eval` を最大 3 回再送して耐性を持たせたが、根本原因（カーネル側の shell タスク飢餓か入力消失か）は未特定。隔離した kill→revive 5 連続ストレスでは再現せず。発生時はベンチが正しく非 0 で終了する。
