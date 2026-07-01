# Relay HA — 複数 relay と中央なしフェイルオーバ

PR #4 構造ギャップ③への回答。relay/relay.c は単一 UDP フォワーダで、
死ねば全ノード間通信が死ぬ SPOF だった。本機構で relay は「複数プロセスを
並べるだけ」で冗長化され、ノード側が決定的に切り替える。wire protocol
(v2: HMAC-SHA256 + nonce window) は不変。

## 規約

- 全ノードが **同一の順序付き relay リスト** を持つ:
  `PKERNEL_RELAY="host[:port],host[:port][,...]"`（最大 4、PSK 共通。
  従来の `PKERNEL_RELAY_HOST/PORT` は 1 件リストとして動き続ける）。
- ルールはひとつ: **「リスト先頭から数えて最初に生きている relay を使う」**。
- 生存判定は既存の `REL_KEEPALIVE` を流用する。relay は受け取った
  KEEPALIVE を送信元へそのまま **エコー（pong）** する — relay 側の唯一の
  追加。同一パケットの反射なので wire format に変更はない。

## 時定数（全ノード共通・arch/linux/\*/net_relay.c）

| 定数 | 値 | 意味 |
|---|---|---|
| `HA_RX_IDLE_MS` | 5 s | 現 relay から無受信ならプローブ送出 |
| `HA_PROBE_TMO_MS` | 3 s | プローブ無応答 → 次の relay へ進み再 REGISTER |
| `HA_FAILBACK_MS` | 10 s | より上位（リスト前方）の relay を定期プローブ |

- **フェイルオーバ**: メッシュ稼働中は受信が絶えないので、relay 死亡は
  「5 s 無受信 → KEEPALIVE プローブ → 3 s 無応答」で最悪 ~8 s で検出。
  `(cur+1) mod N` へ進んで REGISTER し直す。次も死んでいれば同じ手順で
  さらに進む（mod なので全滅中もリストを巡回し、復活を拾う）。
- **フェイルバック**: `cur > 0` の間、リスト前方の relay へ 10 s ごとに
  KEEPALIVE を打つ。エコーが返った瞬間にそこへ戻って REGISTER。
  上位 relay の復活から最悪 ~10 s + RTT で全ノードが先頭へ収束する。

実測（run_relay_failover.sh, localhost, 3 ノード）: kill → 全ノード
フェイルオーバ 8 s、復活 → 全ノードフェイルバック 10 s。

## なぜ中央なしで収束するか

各ノードの選択は **(共有リスト, relay ごとの生死)** の純関数で、
リストは全ノード同一・relay の生死は（収束時間内には）全ノードから
同じに見える。よって調停者なしに全員が同じ relay を選ぶ。一時的に
見え方が割れても（例: フェイルオーバ中の数秒）、フェイルバックの
定期プローブが「先頭の生存 relay」という固定点へ全員を引き戻す。
relay 同士は通信せず、状態も共有しない — ノード表は各 relay が
REGISTER/KEEPALIVE から独立に再学習する（だからこそ再 REGISTER が
切替手順に含まれる）。

## 検証

```
samples/11_distributed/run_relay_failover.sh
```

relay 2 台 (7400/7401) + ノード 3 台で、(1) relay#1 でメッシュ形成、
(2) relay#1 SIGKILL → 15 s 以内に relay#2 で kdds pub/sub 復旧、
(3) relay#1 復活 → 25 s 以内に全ノードが relay#1 へ復帰、を全て
アサートする（失敗時 exit 非 0）。`make -C relay test` の既存 6 本は
そのまま green。

### 既知の別問題（本機構の範囲外・master でも再現）

kdds/pmesh の起動時に、特定方向のルートだけが形成されないまま固着する
ことがある（例: node1→node2 だけ 30 s 経っても届かない。6 方向中 4 方向
のみ成立、という形。master ビルドでも 5 回中 3 回再現 — relay HA 導入
前から存在する pmesh ルート形成のレース）。テストは phase 1 で成立した
方向だけを記録し、その全方向がフェイルオーバ／バック後も流れ続けること
をアサートする設計にしてある。pmesh 側の修正は別タスク。
