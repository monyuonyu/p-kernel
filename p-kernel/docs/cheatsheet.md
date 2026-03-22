# p-kernel シェルコマンド チートシート

## システム

| コマンド | 説明 |
|---------|------|
| `status` | クラスタ全体のヘルスダッシュボード（ノード/メッシュ/トピック/タスク/メモリ） |
| `help` | コマンド一覧 |
| `ver` | カーネルバージョン |
| `mem` | ヒープメモリ使用状況 |
| `ps` | タスク一覧（TID / 優先度 / 状態） |
| `clear` | 画面クリア |

## ネットワーク

| コマンド | 説明 |
|---------|------|
| `net` | NIC ステータス（RTL8139 + 統計） |
| `ping <IP>` | ICMP echo |
| `arp` | ARP キャッシュ表示 |
| `dns <host>` | DNS Aレコード解決 |
| `udp <IP> <port> <msg>` | UDP データグラム送信 |
| `http <host>[/path]` | HTTP GET (port 80) |

## ファイルシステム

| コマンド | 説明 |
|---------|------|
| `ls [path]` | ディレクトリ一覧（デフォルト: `/`） |
| `cat <file>` | ファイル内容表示 |
| `write <file> <text>` | ファイル作成/上書き |
| `rm <file>` | ファイル削除 |
| `mkdir <dir>` | ディレクトリ作成 |
| `cp <src> <dst>` | ファイルコピー |
| `mv <src> <dst>` | ファイル移動/リネーム |
| `exec <file>` | ELF32 バイナリ実行 |
| `mount` | マウントテーブル表示 |

## 共有フォルダ同期 (SFS / port 7381)

| コマンド | 説明 |
|---------|------|
| `sfs list` | `/shared/` ファイル一覧 |
| `sfs stat` | 同期統計 + tombstone 一覧 |
| `sfs push <path>` | ファイルを全ノードへ手動プッシュ |
| `sfs sync` | 全ノードへ SYNC_REQ 送信（起動時同期） |
| `write /shared/F text` | 書き込み → 自動全ノード同期 |
| `rm /shared/F` | 削除 → tombstone 自動伝播 |
| `cp src /shared/F` | コピー → 自動全ノード同期 |

## AI

| コマンド | 説明 |
|---------|------|
| `sensor <t> <h> <p> <l>` | センサーフレーム投入（℃ / % / hPa / lux） |
| `infer <t> <h> <p> <l>` | ローカル MLP 推論 |
| `aistat` | AI 統計（推論数 / レイテンシ） |
| `fl train` | 連合学習ローカルトレーニング |
| `fl status` | FL ラウンド数 + 最終ロス |

## 分散 AI 推論 (DTR / Phase 8)

| コマンド | 説明 |
|---------|------|
| `dtr infer <t> <h> <p> <l>` | パイプライン分散推論（3ステージ） |
| `dtr stat` | パイプライン統計 |

```
Stage 0 (node 0): Embed + Layer0  → dtr/l0 →
Stage 1 (node 1): FFN + Output    → 結果を node 0 へ返送
Stage 2 (node 2): EDF 負荷分散オフロード先
```

## K-DDS pub/sub (port 7376)

| コマンド | 説明 |
|---------|------|
| `topic list` | トピック一覧（名前 / データ長 / seq） |
| `topic pub <name> <data>` | トピックへ発行（全ノードへ pmesh 経由で配信） |
| `topic del <name>` | トピックをクラスタ全体から削除（tombstone） |

## メッシュルーティング (p-mesh / port 7380)

| コマンド | 説明 |
|---------|------|
| `mesh route` | ルーティングテーブル（dst / next_hop / cost / age） |
| `mesh stat` | BEACON 送受信数 / DATA 送受信数 / 中継数 |

```
BEACON: 2秒周期ブロードキャスト（Bellman-Ford で経路自動学習）
DATA  : pmesh_send(node_id) で自動中継（直接届かなくても OK）
```

## クラスタ管理

| コマンド | 説明 |
|---------|------|
| `nodes` | クラスタノード一覧（SWIM 状態） |
| `dproc` | クラスタ全体のプロセス一覧 |
| `kill <name\|path>` | プロセス停止（全ノードへ伝播） |
| `heal list` | Self-Healing ガードタスク一覧 |
| `vital stat` | クラスタ生命兆候一覧（K-DDS vital/* トピック） |
| `edf stat` | SLA 統計 + ノード負荷 |
| `replica stat` | 状態複製統計 |

## 永続化 (port FAT32)

| コマンド | 説明 |
|---------|------|
| `persist list` | ディスク保存済みトピック一覧 |
| `persist save` | 全トピックを今すぐ保存 |
| `persist clear` | 保存済みトピックを全削除 |

## 分散プリミティブ（分散モード限定）

| コマンド | 説明 |
|---------|------|
| `dtask <n> <fn>` | ノード n にタスク作成（fn: hello / counter） |
| `dsem new` | 分散セマフォ作成 |
| `dsem wai <0xID>` | 分散セマフォ wait |
| `dsem sig <0xID>` | 分散セマフォ signal |
| `infer <n> <t> <h> <p> <l>` | ノード n でリモート推論 |

---

## ネットワークポート一覧

| ポート | プロトコル | 用途 |
|-------|-----------|------|
| 7374 | DRPC | 分散 RPC + ノード発見（UDP 直接） |
| 7375 | SWIM | gossip 生存監視（UDP 直接） |
| 7376 | K-DDS | pub/sub（pmesh 経由） |
| 7379 | REPLICA | 状態複製（pmesh 経由） |
| 7380 | p-mesh | メッシュルーティング（UDP 直接） |
| 7381 | SFS | 共有フォルダ同期（pmesh 経由） |

## 3ノードクラスタ起動

```sh
cd boot/x86
cp disk.img node0.img && cp disk.img node1.img && cp disk.img node2.img

# 別ターミナルで
make run-node0   # node 0  MAC=52:54:00:00:00:01  IP=10.1.0.1
make run-node1   # node 1  MAC=52:54:00:00:00:02  IP=10.1.0.2
make run-node2   # node 2  MAC=52:54:00:00:00:03  IP=10.1.0.3
```

ノード ID は MAC アドレス末尾オクテット - 1 で自動決定。再コンパイル不要。
