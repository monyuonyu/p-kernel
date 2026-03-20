# x86 アーキテクチャ実装

micro T-Kernel 2.0 の x86/QEMU ポートです。
OS コア・デバイスドライバ・完全 TCP/IP ネットワークスタック・
**AI 時代のカーネルプリミティブ（Tensor / AI Job / ゼロコピーパイプライン / 連合学習）**・
**分散 RPC（DRPC）** が動作します。

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
| `shell.c` | インタラクティブシェル（AI コマンド含む） |
| `pci.c` | PCI コンフィグ空間列挙（0xCF8/0xCFC） |
| `rtl8139.c` | RTL8139 NIC ドライバ |
| `netstack.c` | 完全 TCP/IP スタック |
| `drpc.c` | 分散 T-Kernel RPC over UDP（ハートビート・タスク生成・セマフォ・推論） |

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
| `vga.h` | VGA API・カラー定数 |
| `keyboard.h` | キーボード API |
| `pci.h` | PCI API・ベンダー ID 定数 |
| `rtl8139.h` | NIC API・統計変数 |
| `netstack.h` | TCP/IP 全構造体・API |

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

### QEMU 2 ノード構成

```
端末 0:  make run-node0  →  Node 0  IP=10.1.0.1  MAC=52:54:00:00:00:01
端末 1:  make run-node1  →  Node 1  IP=10.1.0.2  MAC=52:54:00:00:00:02

ネットワーク: UDP マルチキャスト 230.0.0.1:1234（仮想 Ethernet ハブ）
```

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
make debug            # GDB リモートデバッグ（:1234）
```

## 既知の制限事項

- TCP は単一接続のみ（`TCP_MAX_CONN = 2`）
- TCP セグメント分割なし（1 回の `tcp_write` で最大 1460 bytes）
- TCP 再送なし（パケットロスで接続失敗）
- HTTPS（TLS）非対応
- MLP は demo-grade（本番 FL には差分プライバシー・安全な集約が必要）
