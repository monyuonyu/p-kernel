# x86 アーキテクチャ実装

micro T-Kernel 2.0 の x86/QEMU ポートです。
OS コア・デバイスドライバ・完全 TCP/IP ネットワークスタック・
**AI 時代のカーネルプリミティブ（Tensor / AI Job / ゼロコピーパイプライン / 連合学習）**・
**分散 RPC（DRPC）**・**K-DDS（カーネルネイティブ Data Distribution Service）**・
**SFS（Shared Folder Sync / 共有フォルダ同期）** が動作します。

## 動作確認済み機能

```
# ネットワーク
p-kernel> ping 10.0.2.2
[icmp] echo REPLY from 10.0.2.2  id=20480  seq=1

p-kernel> http example.com/
DNS: example.com -> 104.18.26.120
[tcp] ESTABLISHED
HTTP/1.1 200 OK  ...  --- 1571 bytes ---

# AI カーネル（シングルノード）
p-kernel> sensor 22 50 1013 500
[sensor] pushed (t=22C h=50% p=1013hPa l=500lux)
[ai]  infer: normal    (t=22C)

p-kernel> infer 45 85 950 2000
[infer] local -> CRITICAL

p-kernel> fl train
[FL] local train step...
[FL] aggregate OK

# 分散推論（2 ノード）
node0> infer 1 45 85 950 2000
[dtk_infer] -> node 1
[infer] node 1 -> CRITICAL

# 共有フォルダ同期（3 ノード）— Phase 9.5
node0> mkdir /shared
[mkdir] created: /shared
node0> write /shared/hello.txt hello world
[write] ok: /shared/hello.txt
[sfs] pushed "/shared/hello.txt"  chunks=1
# node1/node2 では自動受信
[sfs] received "/shared/hello.txt"  13 bytes

node1> rm /shared/hello.txt
[rm] deleted: /shared/hello.txt
[sfs] delete broadcast: "/shared/hello.txt"
# node0/node2 に伝播
[sfs] deleted (remote): "/shared/hello.txt"
```

## ファイル構成

### OS・ドライバ層

| ファイル | 機能 |
|---------|------|
| `cpu_init.c` | CPU 初期化、GDT 設定 |
| `cpu_support.S` | コンテキストスイッチ、SVC ハンドラ |
| `tkdev_init.c` | T-Kernel デバイス初期化 |
| `inittask_def.c` / `inittask_main.c` | 初期タスク定義・起動 |
| `usermain.c` | ユーザーメインタスク（AI カーネル初期化・ドライバ起動・ネット初期化） |
| `sio.c` | COM1 シリアル I/O（送受信） |
| `vga.c` | VGA テキストモードドライバ（80×25、スクロール対応） |
| `keyboard.c` | PS/2 キーボードドライバ（IRQ1、US スキャンコード→ASCII） |
| `shell.c` | インタラクティブシェル（AI・SFS コマンド含む） |
| `pci.c` | PCI コンフィグ空間列挙（0xCF8/0xCFC） |
| `rtl8139.c` | RTL8139 NIC ドライバ |
| `netstack.c` | 完全 TCP/IP スタック |
| `drpc.c` | 分散 T-Kernel RPC over UDP（ハートビート・タスク生成・セマフォ・推論） |
| `persist.c` | FAT32 永続化（定期チェックポイント・ブート時リストア） |

### 分散システム層（Phase 5〜9.5）

| ファイル | 機能 |
|---------|------|
| `swim.c` | SWIM gossip 生存監視プロトコル（UDP port 7375） |
| `kdds.c` | K-DDS — カーネルネイティブ Data Distribution Service（port 7376、32 トピック） |
| `replica.c` | gossip ベーストピックテーブル全複製（port 7379、3 秒周期） |
| `heal.c` | 自己修復タスク（ノード障害検出・自動再起動） |
| `edf.c` | EDF リアルタイムスケジューラ（負荷分散） |
| `vital.c` | バイタルサイン発行（各ノードの生存トピック） |
| `dtr.c` | 分散 Transformer 推論（Pipeline Parallelism、3 ステージ） |
| `dproc.c` | 分散プロセスレジストリ（K-DDS topic "proc/N"） |
| `sfs.c` | **Shared Folder Sync** — `/shared/` をノード間で UDP 同期（port 7381） |

### AI カーネルプリミティブ

| ファイル | 機能 |
|---------|------|
| `tensor.c` | カーネル管理テンソル（バンプアロケータ、16 スロット×16 KB、32 バイトアライン） |
| `ai_job.c` | ソフトウェア NPU（4→8→8→3 MLP クラシファイア、ジョブキュー、ワーカータスク） |
| `pipeline.c` | ゼロコピーパイプライン（16 フレーム、セマフォ同期リングバッファ） |
| `fedlearn.c` | 連合学習 FedAvg（有限差分勾配近似 + DRPC 集約） |
| `ai_stats.c` | AI グローバル統計シングルトン、`ai_kernel_init()` |

### ヘッダー (`include/`)

| ファイル | 内容 |
|---------|------|
| `ai_kernel.h` | Tensor / AI Job / Pipeline / FL / 統計の全 API 宣言 |
| `drpc.h` | 分散 RPC プロトコル定義、ノードテーブル、分散 API |
| `kdds.h` | K-DDS API（32 トピック、64 ハンドル、QoS ポリシー） |
| `replica.h` | gossip 複製プロトコル定義、REPLICA_PKT 構造体 |
| `sfs.h` | SFS API（`sfs_push` / `sfs_delete` / `sfs_boot_sync` 等） |
| `vga.h` | VGA API・カラー定数 |
| `keyboard.h` | キーボード API |
| `pci.h` | PCI API・ベンダー ID 定数 |
| `rtl8139.h` | NIC API・統計変数 |
| `netstack.h` | TCP/IP 全構造体・API |
| `utk_config_depend.h` | カーネル定数（`CFN_MAX_SEMID=48` 等） |

---

## AI カーネル詳細

### MLP センサー分類器（4→8→8→3）

```
入力 (int8 Q8):
  temp      (°C - 20) × 2   / 127
  humidity  (% - 50)  × 2   / 127
  pressure  (hPa-1013) / 2  / 127
  light     (lux-500)  / 4  / 127
          ↓ L1: Linear + ReLU (×8)
          ↓ L2: Linear + ReLU (×8)
          ↓ L3: Linear → argmax
出力: 0=normal / 1=alert / 2=critical
```

### ゼロコピーパイプライン

```
producer (shell/sensor) → pipeline_push()
                                ↓  SENSOR_FRAME を ring[head] に書き込み（コピーなし）
                          data_sem signal
                                ↓
consumer (ai_infer_task) ← pipeline_pop()
                          同じ ring[tail] を読み出し → mlp_forward()
```

セマフォ 3 本：`data_sem`（充填スロット数）/ `space_sem`（空きスロット数）/ `lock_sem`（head/tail ミューテックス）

### 連合学習（FedAvg）

```
fl_local_train()        有限差分でバイアス勾配を近似
        ↓ delta_b3[3]
dtk_fl_aggregate()      シングルノード: ローカル適用
                        マルチノード : DRPC で node 0 に集約 → 全ノードへ反映
```

### センサー値エンコーディング（DRPC ペイロード）

DRPC_PKT の arg[0] に 4 つの int8 をパック（32 ビット）:

```c
#define SENSOR_PACK(t,h,p,l) \
    ( ((W)(B)(t)<<24) | ((W)(UB)(h)<<16) | ((W)(UB)(p)<<8) | (UB)(l) )
```

---

## SFS（Shared Folder Sync）詳細 — Phase 9.5

`/shared/` パス以下のファイルをクラスタ全ノードへリアルタイム同期します。

### プロトコル仕様

| 項目 | 値 |
|-----|---|
| トランスポート | Raw UDP、port **7381** |
| チャンクサイズ | 512 bytes |
| 最大ファイルサイズ | 32 KB |
| 対象パス | `/shared/` のみ |
| 削除伝播 | Tombstone テーブル（最大 16 エントリ） |

### パケット構造 (`SFS_PKT`, 596 bytes)

```
magic(4) + version(1) + type(1) + src_node(1) + _pad(1) +
path[64] + total_size(4) + chunk_idx(4) + chunk_len(4) + _pad2(4) +
data[512]
```

### パケットタイプ

| タイプ | 説明 |
|-------|------|
| `SFS_START` | ファイル転送開始（total_size 通知） |
| `SFS_CHUNK` | チャンクデータ送信 |
| `SFS_DELETE` | ファイル削除（tombstone 伝播） |
| `SFS_SYNC_REQ` | ブート時同期要求（全ファイルを push してもらう） |

### シェルコマンド

```
sfs list              — /shared/ の一覧表示
sfs stat <path>       — ファイルのステータス確認
sfs push <path>       — 手動でファイルを全ノードへ送信
sfs sync              — SYNC_REQ ブロードキャスト（ブート時と同じ）
```

`write`・`cp` コマンドは `/shared/` パスを自動検出して `sfs_push()` を呼び出します。
`rm` コマンドは `sfs_delete()` を呼び出してリモートの tombstone を伝播します。

---

## 分散 RPC (DRPC) 詳細

### プロトコル

| 種別 | 説明 |
|-----|------|
| HEARTBEAT | 500 ms ブロードキャスト → ノード発見・死活監視 |
| REQ/REPLY | 同期 RPC（3 秒タイムアウト） |

### ノード状態機械

```
UNKNOWN → ALIVE → SUSPECT → DEAD
                ↑_________↑  (ハートビート受信で ALIVE に戻る)
```

### 分散 API

| 関数 | 説明 |
|-----|------|
| `dtk_cre_tsk(node, func_id, pri)` | 任意ノードにタスク生成 |
| `dtk_cre_sem(isemcnt)` | グローバルセマフォ生成 |
| `dtk_sig_sem(gsemid, cnt)` | セマフォ signal（リモートルーティング） |
| `dtk_wai_sem(gsemid, cnt, t)` | セマフォ wait |
| `dtk_infer(node, packed, &cls, tmout)` | 任意ノードで MLP 推論（透過ルーティング） |

### QEMU 3 ノード構成

```
端末 0:  make run-node0  →  Node 0  IP=10.1.0.1  MAC=52:54:00:00:00:01
端末 1:  make run-node1  →  Node 1  IP=10.1.0.2  MAC=52:54:00:00:00:02
端末 2:  make run-node2  →  Node 2  IP=10.1.0.3  MAC=52:54:00:00:00:03

ネットワーク: UDP マルチキャスト 230.0.0.1:1234（仮想 Ethernet ハブ）
ディスクイメージ: ノードごとに独立（node0.img / node1.img / node2.img）
```

ノード ID は MAC アドレスの最終オクテットから自動決定 — 再コンパイル不要。

---

## ネットワークスタック詳細

### プロトコル実装

| プロトコル | 実装内容 |
|-----------|---------|
| **Ethernet** | フレーム送受信、型によるディスパッチ |
| **ARP** | Request/Reply、16 エントリキャッシュ、GARP |
| **IP** | ヘッダーチェックサム（一の補数）、TTL、フラグメントなし |
| **ICMP** | Echo Request → Reply、Echo Reply 受信ログ |
| **UDP** | 8 ソケット、ポートディスパッチ、checksum=0（IPv4 optional） |
| **DNS** | UDP A レコードクエリ、3 秒タイムアウト、ARP リトライ付き |
| **TCP** | 三方向ハンドシェイク、状態機械、チェックサム、FIN/RST 処理 |
| **HTTP** | GET リクエスト、チャンク対応、接続クローズ検出 |

---

## QEMU 実行環境

```sh
cd p-kernel/boot/x86

make run              # シングルノード（VGA + シリアル + RTL8139 NIC）
make run-headless     # ヘッドレス
make run-node0        # 分散モード node 0
make run-node1        # 分散モード node 1（別ターミナル）
make run-node2        # 分散モード node 2（別ターミナル）
make debug            # GDB リモートデバッグ（:1234）
```

## 既知の制限事項

- TCP は単一接続のみ（`TCP_MAX_CONN = 2`）
- TCP セグメント分割なし（1 回の `tcp_write` で最大 1460 bytes）
- TCP 再送なし（パケットロスで接続失敗）
- HTTPS（TLS）非対応
- MLP は demo-grade（本番 FL には差分プライバシー・安全な集約が必要）
