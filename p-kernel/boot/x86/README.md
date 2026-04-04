# p-kernel

# p-kernel — 方舟のようなOS

- ノードが落ちても自動で縮退し、最後の一台になっても止まらない
- その一台の中に、できる限りの情報を保管する
- ネットワークが不安定でも、自律的に動く
- 仲間のノードを見つけたら自動で繋がり、知識を同期する
- AIはアプリではなくカーネルの一部として、OSと一体で生きている

通常のOSはアプリケーションを動かすために動作するが、
このOSはAIが生存できるように設計する

---

# boot/x86 — p-kernel x86/QEMU 統合ビルド

micro T-Kernel 2.0 + 完全 TCP/IP スタック + **AI カーネルプリミティブ** +
**分散 RPC（DRPC）** + **K-DDS** + **SFS（Shared Folder Sync）** を含む
x86/QEMU 向けシングルバイナリのビルドディレクトリです。

## クイックスタート

```sh
cd p-kernel/boot/x86

# ビルド
make

# シングルノード実行（VGA + シリアル stdio + RTL8139 NIC）
make run

# ヘッドレス実行
make run-headless

# 分散 3 ノード（別ターミナルで全て起動）
make run-node0   # 端末 1: node 0  IP=10.1.0.1
make run-node1   # 端末 2: node 1  IP=10.1.0.2
make run-node2   # 端末 3: node 2  IP=10.1.0.3

# 注意: 分散モードはノードごとに独立したディスクイメージが必要
cp disk.img node0.img && cp disk.img node1.img && cp disk.img node2.img

# シリアル出力をファイルに保存
make run-log && cat serial.log

# GDB リモートデバッグ（別ターミナルで gdb -ex "target remote :1234"）
make debug
```

## 起動シーケンス

```
QEMU -kernel bootloader.bin
  │
  ├─ [start.S]      Multiboot エントリ（32 bit）
  │                 32→64 ビットロングモード移行
  │                 ページテーブル（1GB アイデンティティマッピング）
  │
  ├─ [main.c]       IDT 初期化
  │                 物理メモリ管理初期化
  │                 8259A PIC 初期化
  │                 T-Kernel 起動 → tkstart()
  │
  └─ [usermain.c]   ai_kernel_init()           ← Tensor / AI Job / Pipeline 初期化
                    AI ワーカータスク起動（priority 6）
                    AI 推論タスク起動（priority 7）
                    PS/2 キーボード初期化（IRQ1）
                    シェルタスク起動（priority 2）
                    RTL8139 NIC 初期化（IRQ11）
                    DRPC 初期化（分散 MAC 検出時）
                    SWIM / K-DDS / replica / heal タスク起動
                    EDF / vital / DTR / dproc タスク起動
                    sfs_init() + sfs_boot_sync()   ← SFS 共有フォルダ同期
                    persist 初期化・リストア
                    ネットスタック起動（ARP 事前送信）
                    ネット RX タスク起動（priority 3）
```

## ビルド構成

Makefile は以下の 4 つのソースグループを統合してリンクします。

| グループ | ソースパス | 内容 |
|---------|---------|------|
| boot/x86 | `./` | start.S、main.c、idt.c、memory.c、pic.c、timer.c、isr.S |
| arch/x86 | `../../arch/x86/` | CPU 初期化、シリアル、VGA、キーボード、シェル、PCI、RTL8139、netstack、drpc、swim、kdds、replica、heal、edf、vital、dtr、dproc、**sfs**、persist、**tensor、ai_job、pipeline、fedlearn、ai_stats** |
| kernel/common | `../../kernel/common/` | T-Kernel コア（タスク・メモリ・同期・タイマー等） |
| lib/libc/string | `../../lib/libc/string/` | memset・memcpy・strcmp 等 |

## make ターゲット一覧

| ターゲット | 説明 |
|-----------|------|
| `make` | `bootloader.bin` をビルド |
| `make run` | QEMU 実行（VGA + シリアル + NIC） |
| `make run-headless` | ヘッドレス実行（シリアルのみ） |
| `make run-nonet` | NIC なしで実行 |
| `make run-log` | シリアル出力を `serial.log` に保存 |
| `make run-node0` | 分散モード node 0（UDP マルチキャスト、node0.img 使用） |
| `make run-node1` | 分散モード node 1（UDP マルチキャスト、node1.img 使用） |
| `make run-node2` | 分散モード node 2（UDP マルチキャスト、node2.img 使用） |
| `make debug` | GDB リモートデバッグ（ポート 1234） |
| `make clean` | ビルド生成物を削除 |

## 主要ファイル

| ファイル | 役割 |
|---------|------|
| `start.S` | Multiboot ヘッダー、32→64 ビット移行、ページテーブル構築 |
| `main.c` | C エントリ、IDT・PIC・タイマー・T-Kernel 起動 |
| `idt.c` / `isr.S` | 割り込み記述子テーブル、ISR スタブ、IRQ ディスパッチテーブル |
| `memory.c` | E820 物理メモリマップ解析 |
| `pic.c` | 8259A PIC 再マッピング（IRQ0-7 → INT 32-39） |
| `timer.c` | PIT タイマー（T-Kernel システムクロック） |
| `linker.ld` | メモリレイアウト定義（カーネル 0x100000 〜） |
| `Makefile` | 統合ビルドシステム |

## ハードウェア構成（QEMU）

### シングルノード

```
CPU   : qemu64（-cpu qemu64）
MEM   : 128 MB（QEMU デフォルト）
シリアル: COM1 (0x3F8) → stdio または serial.log
NIC   : RTL8139 (I/O=0xC000, IRQ=11, MAC=52:54:00:12:34:56)
NW    : ユーザーモード（slirp）
          ゲスト  : 10.0.2.15
          GW     : 10.0.2.2
          DNS    : 10.0.2.3
```

### 分散 3 ノード（UDP マルチキャスト）

```
Node 0: MAC=52:54:00:00:00:01  IP=10.1.0.1  disk=node0.img
Node 1: MAC=52:54:00:00:00:02  IP=10.1.0.2  disk=node1.img
Node 2: MAC=52:54:00:00:00:03  IP=10.1.0.3  disk=node2.img
NW    : socket mcast=230.0.0.1:1234（仮想 Ethernet ハブ）

ポート割り当て:
  DRPC   : UDP 7374   分散 RPC
  SWIM   : UDP 7375   gossip 生存監視
  K-DDS  : UDP 7376   Data Distribution Service
  replica: UDP 7379   トピック状態複製
  SFS    : UDP 7381   共有フォルダ同期
```

同一バイナリを MAC アドレスで自動識別 — 再コンパイル不要。
各ノードは独立したディスクイメージを使用（ロック競合回避）。

## 動作確認済みの出力

### シングルノード

```
[ai]   Tensor pool   : 16 slots × 16 KB
[ai]   AI job queue  : 8 slots (software NPU)
[ai]   Pipeline      : 16 frames zero-copy
[ai]   MLP model     : 4→8→8→3 sensor classifier
[OK]  AI worker task
[OK]  AI infer task
[OK]  Keyboard (IRQ1)
[OK]  Shell task
[net] RTL8139 ready  I/O=0xC000  IRQ=11  MAC=52:54:00:12:34:56

p-kernel> sensor 22 50 1013 500
[sensor] pushed (t=22C h=50% p=1013hPa l=500lux)
[ai]  infer: normal    (t=22C)

p-kernel> infer 45 85 950 2000
[infer] local -> CRITICAL

p-kernel> fl train
[FL] local train step...
[FL] aggregate OK
```

### 2 ノード分散推論

```
# node 0
[drpc] node 1 discovered  IP=10.1.0.2
p-kernel> infer 1 45 85 950 2000
[dtk_infer] -> node 1
[infer] node 1 -> CRITICAL

# node 1（node 0 からのリクエストを受信）
[drpc/infer] from node 0  class=2
```

### 3 ノード SFS 共有フォルダ同期（動作確認済み）

```
# node 0: ファイル作成 → 自動ブロードキャスト
[drpc] node 1 discovered  IP=10.1.0.2
[drpc] node 2 discovered  IP=10.1.0.3
p-kernel> mkdir /shared
[mkdir] created: /shared
p-kernel> write /shared/hello.txt hello world
[write] ok: /shared/hello.txt
[sfs] pushed "/shared/hello.txt"  chunks=1

# node 1 / node 2（自動受信）
[sfs] START "/shared/hello.txt"  size=13
[sfs] received "/shared/hello.txt"  13 bytes

# node 1: 削除 → tombstone 伝播
p-kernel> rm /shared/hello.txt
[rm] deleted: /shared/hello.txt
[sfs] delete broadcast: "/shared/hello.txt"

# node 0 / node 2（削除伝播受信）
[sfs] deleted (remote): "/shared/hello.txt"
```

## 実装フェーズ一覧

| フェーズ | 名称 | 状態 | 概要 |
|---------|------|------|------|
| Phase 1  | x86 ポート | ✅ 完成 | micro T-Kernel 2.0 を x86/QEMU へ移植。Multiboot、IDT、PIC、PIT、シリアル、VGA |
| Phase 2  | K-DDS | ✅ 完成 | カーネルネイティブ pub/sub。「すべてはトピック」。ローカル＆分散透過 API |
| Phase 3  | Self-Healing | ✅ 完成 | ノード DEAD 検出 → 後継ノードがカーネルタスクを自動引き継ぎ (heal.c) |
| Phase 4  | DRPC | ✅ 完成 | 分散 RPC。dtk_cre_tsk / dtk_infer / dtk_fl_aggregate |
| Phase 5  | SWIM + Replica | ✅ 完成 | gossip 生存監視 + 全トピックスナップショット複製。脱中央集権型HA |
| Phase 6  | EDF + Vital | ✅ 完成 | Earliest Deadline First スケジューラ + クラスタ生命兆候モニタ |
| Phase 7  | 永続化 + ELF Watchdog | ✅ 完成 | FAT32 チェックポイント + heal による ring-3 デーモン自動再起動 |
| Phase 8  | 分散 Transformer | ✅ 完成 | MHSA(h=2,dk=4)+FFN+Cls。縮退モード連携分散推論 (→ Phase 11 で拡張) |
| Phase 9  | dproc | ✅ 完成 | 分散プロセスレジストリ。kill / failover がクラスタ全体に伝播 |
| Phase 9.5 | SFS | ✅ 完成 | /shared/ フォルダをゴシップで全ノード同期。tombstone 削除伝播 |
| Phase 10 | pmesh + kloader | ✅ 完成 | メッシュルーティング + ring-3 カーネルローダーデーモン (kloader_d.elf) |
| Phase 11 | 縮退モード | ✅ 完成 | ノード数に応じて自動縮退。SOLO=ローカル / REDUCED=TensorPar / FULL=Pipeline |
| **Phase 12** | **大規模 LLM 推論** | 🔲 設計済み | ring-3 推論デーモン群 + ring-0 制御。200B 級モデルへの道筋 |
| **Phase 13** | **DMN** | ✅ 完成 | Default Mode Network。外部刺激で発火、アイドル時に記憶整理 |
| **Phase 14** | **無意識学習 (GA)** | 🔲 設計中 | DMN アイドル中に遺伝的アルゴリズムで Transformer 重みを自己改善 |
| **Phase 15** | **無意識学習 (RL)** | 🔲 設計中 | DMN 自身の行動方針を強化学習で更新。カーネルが自分の動き方を学ぶ |
| **Phase 16** | **kloader — ブート分離** | ✅ 完成 | カーネルとローダーを分離。kloader.bin が FAT32/ネットから kernel.elf を取得・ELF展開して起動 |

---

## Phase 14/15 — 無意識学習

人間の学習の大半は、意識していない間に起きる。
睡眠中に記憶が定着し、繰り返しの中でパターンが体に染み込む。

p-kernel では DMN のアイドル時間がその「眠り」にあたる。
外部からの刺激がない間、カーネルは静かに自分自身を書き換える。

```
外部刺激あり (ACTIVE)
  → 推論・通信を優先。学習はしない

外部刺激なし (IDLE) — ここが「無意識」
  Phase 14: GA が Transformer の重みを数世代だけ進化させる
            精度が上がった重みだけを残す。悪化したら捨てる
  Phase 15: RL が DMN 自身の行動方針を更新する
            「いつ IDLE に入るか」「何を整理するか」を報酬で学ぶ
```

意識 (推論・応答) は ring-0 の dtr.c が担う。
無意識 (学習・最適化) は DMN のアイドル時間が担う。

この分離が、このカーネルのコアにある考え方。

### Phase 14 — GA による Transformer 重みの自己改善

- **何を進化させるか**: `W_q`, `W_k`, `W_v`, `W_o`, FFN の重み (568 floats)
- **適応度**: 直近の推論ログに対する分類精度
- **世代管理**: アイドル 1 回につき数世代だけ回す (重くしない)
- **実装場所**: `dmn_idle_work()` の中で `ga_step()` を呼ぶ
- **保存**: 改善した重みは K-DDS トピック `"ai/weights"` に publish → persist で永続化

```
dmn_idle_work()
  └─ ga_step()
       ├── 現在の重みに小さな変異を加えた個体を N 個生成
       ├── 直近の入出力ログで各個体の精度を評価
       ├── 最も精度が高い重みを採用
       └── kdds_pub("ai/weights", ...) → persist で保存
```

### Phase 15 — RL による DMN 自身の方針学習

- **状態**: `(idle_duration, degrade_level, trigger_rate, inference_latency)`
- **行動**: `(idle_threshold を増減, 整理処理の優先順位を変更)`
- **報酬**: 次の推論リクエストに答えるまでのレイテンシ (低いほど良い)
- **実装**: シンプルな Q テーブル (状態数が少ないので十分)

```
DMN が IDLE → ACTIVE に戻るたびに報酬を計算
  → Q テーブルを更新
  → 次の IDLE 時の行動方針に反映
```

### 実装タスク

- [ ] `ga.c` — 遺伝的アルゴリズム基盤 (変異・選択・適応度評価)
- [ ] `rl.c` — Q 学習基盤 (状態・行動・報酬・Q テーブル更新)
- [ ] `dtr.c` 拡張 — 推論ログ (入力/出力) を ring バッファに記録
- [ ] `dmn.c` 拡張 — アイドル時に `ga_step()` / `rl_update()` を呼ぶ
- [ ] K-DDS `"ai/weights"` トピック + persist 連携で重みを永続化

---

## Phase 13 — DMN (Default Mode Network)

人間の脳はつねに動いている。外的な刺激があれば反応し、何もない時は内側で情報を整理する。
このフェーズではその仕組みをカーネルに組み込む。

### 設計

```
外部刺激 (センサー / ネットワーク / K-DDS トピック)
  → dmn_trigger() で発火
  → dtr_infer() を優先実行

アイドル状態 (一定時間リクエストなし)
  → dmn_idle_task() が起動
  → replica の整理 / KV キャッシュの LRU 再構築 / K-DDS 統計集計
  → fedlearn の勾配集約 (学習の定着)
```

### 主なコンポーネント

| モジュール | 役割 |
|---|---|
| `dmn.c` | cyclic handler でハートビートを刻み、アイドル検知と整理タスクを管理 |
| `dmn_trigger()` | 外部刺激を受け取り推論を優先起動。swim / drpc / kdds から呼ばれる |
| `dmn_idle()` | アイドル時の整理処理。記憶の定着・不要エントリの削除・索引再構築 |

### カーネルへの組み込み

- `tk_cre_cyc` (T-Kernel cyclic handler) でハートビート周期を設定
- 外部刺激到着時は event flag でアイドルを中断し推論を優先
- 整理処理は最低優先度タスクとして動作し、推論の邪魔をしない

---

## Phase 12 — 大規模 LLM 推論デーモン設計

### コンセプト: 「AIの魂は ring-0、AIの体は ring-3」

現在の dtr.c (ring-0) は d_model=8 の Transformer を動かせるが、
200B パラメータ (fp16 で 400 GB) はカーネルの静的領域には絶対に入らない。

**解決策: kloader_d と同じ発想を AI 推論に適用する。**

```
ring-0 (カーネル) — "AIの魂"           ring-3 (ユーザー空間) — "AIの体"
─────────────────────────────────       ────────────────────────────────────
dtr.c : 推論スケジューラ・制御          infer_d.elf   : 実際の行列演算
K-DDS : ノード間テンソル転送バス        weights_d.elf : 重みの mmap 管理
DRPC  : 分散パイプライン調整            kvcache_d.elf : KV キャッシュ管理
heal  : デーモン全体の watchdog         ← heal が死んでも即再起動
tensor.c : 低レイヤ演算プリミティブ     ← p_syscall 経由でカーネルを呼ぶ
↑ 絶対に死なせない                      ↑ 死んでも heal が復活させる
```

### 新しいデーモン構成 (ring-3)

| デーモン | 役割 |
|---------|------|
| `infer_d.elf` | 実際の大行列演算。大きなスタック (数 MB) を動的確保。p_syscall でカーネル tensor プリミティブを呼ぶ |
| `weights_d.elf` | 重みファイルを VFS 経由でページ単位に読み込み。LRU キャッシュで VRAM 相当を管理 |
| `kvcache_d.elf` | Transformer の KV キャッシュを管理。マルチリクエスト並列処理 |

init.rc での登録:
```sh
guard /weights_d.elf    # heal watchdog: 重みローダー
guard /infer_d.elf      # heal watchdog: 推論エンジン
guard /kvcache_d.elf    # heal watchdog: KV キャッシュ
```

### 分散推論アーキテクチャ (200B 級)

```
Tensor Parallel × Pipeline Parallel のハイブリッド:

  ┌─────────────────────────────────────────────────────────┐
  │  p-kernel クラスタ (8 ノード例)                          │
  │                                                         │
  │  [Node 0,1] Stage 0: Embed + Layers 0-11  (PP Stage 0) │
  │    └─ Node 0: head 0,1,2  (TP shard 0)                 │
  │    └─ Node 1: head 3,4,5  (TP shard 1)                 │
  │                    ↓ K-DDS "dtr/pp0"                   │
  │  [Node 2,3] Stage 1: Layers 12-23         (PP Stage 1) │
  │    └─ Node 2: head 0,1,2  (TP shard 0)                 │
  │    └─ Node 3: head 3,4,5  (TP shard 1)                 │
  │                    ↓ K-DDS "dtr/pp1"                   │
  │  [Node 4,5] Stage 2: Layers 24-35         (PP Stage 2) │
  │  [Node 6,7] Stage 3: Layers 36-47 + Cls  (PP Stage 3)  │
  └─────────────────────────────────────────────────────────┘

  各ノードの infer_d.elf が実際の行列演算を担当。
  dtr.c (ring-0) がパイプライン制御・障害検出・再ルーティングを担当。
```

### スケール別の対応モデル規模

| ノード数 | 縮退レベル | 対応モデル規模 | 分散方式 |
|---------|----------|-------------|---------|
| 1 | SOLO | < 1B params (RAM に収まる分) | ローカル infer_d |
| 2 | REDUCED | < 4B params | Tensor Parallel (head 分割) |
| 4 | FULL | < 16B params | TP × PP ハイブリッド |
| 8+ | FULL | 70B〜200B params | TP × PP 多段 + 量子化 |

### Phase 12 実装タスク

```
[ ] p_syscall 拡張 (0x230 SYS_INFER_SUBMIT, 0x231 SYS_INFER_WAIT)
    → ring-3 から dtr.c のパイプラインを呼ぶ
[ ] infer_d.elf の実装
    → 大スタック (malloc 相当) + tensor 演算 + VFS 重みロード
[ ] weights_d.elf の実装
    → mmap 的ページング、LRU キャッシュ、VFS ストリーミング読み込み
[ ] kvcache_d.elf の実装
    → KV キャッシュ + ページング + マルチリクエスト管理
[ ] dtr.c 拡張
    → PP Stage 数を動的設定、infer_d への計算委託インターフェース
[ ] int8/fp16 量子化サポート
    → tensor.c に quantize/dequantize プリミティブ追加
[ ] ベンチマーク
    → tokens/sec の計測、3ノードで 7B モデルを動かす実証
```

---

## Phase 16 — kloader: カーネル/ローダー分離

### コンセプト

通常の OS では「GRUB がカーネルをロードする」が当たり前だが、p-kernel には
より強い動機がある。**ローダーが生きていれば、カーネルを外から受け取って再起動できる。**

```
従来:  QEMU -kernel bootloader.bin  ← カーネル自身が Multiboot を持つ
Phase 16:
  QEMU -kernel kloader.bin          ← 小さなローダー (8 KB, 0x50000)
    ├─ FAT32 disk → KERNEL.ELF を読み込む
    ├─ (Phase 16b) ネット受信 → 別ノードから kernel ELF を取得して起動
    └─ ELF展開 → e_entry にジャンプ → p-kernel が通常起動
```

### ファイル構成

| ファイル | 役割 |
|---------|------|
| `kloader_start.S` | Multiboot ヘッダー + エントリ。EAX/EBX (MB magic/info) を保存して kl_main へ |
| `kl_main.c` | FAT32 試行 → ネット試行 → 失敗時 halt。カーネル起動時に EAX/EBX を復元 |
| `kl_fat32.c` | IDE PIO + FAT32 BPB 解析 + ファイル読み込み + ELF32 ロード |
| `kl_net.c` | ネットワーク受信 (Phase 16b: RTL8139 minimal + UDP) |
| `kloader.ld` | kloader を 0x50000 に配置 |
| `kernel.ld` | kernel.elf: `.multiboot` を除去し `_start` を 0x100000 の先頭に配置 |

### ビルドと実行

```sh
# ビルド (kloader.bin + kernel.elf + disk.img)
make all

# kloader 経由で起動 (推奨: 本番に近い構成)
make run-kloader

# 従来の直接起動 (後方互換: 開発・デバッグ用)
make run
```

### 起動シーケンス (Phase 16)

```
QEMU -kernel kloader.bin
  │
  ├─ [kloader_start.S]  Multiboot エントリ (0x50000)
  │                     EAX=0x2BADB002, EBX=MB info → kl_main へ渡す
  │
  ├─ [kl_main.c]        シリアル初期化
  │                     FAT32 → KERNEL.ELF 発見
  │
  ├─ [kl_fat32.c]       IDE PIO → FAT32 解析 → ファイル読み込み (staging: 0x300000)
  │                     ELF32 ヘッダ解析 → PT_LOAD セグメントを vaddr へ展開
  │                     e_entry を返す
  │
  └─ [kl_main.c]        EAX/EBX を復元して e_entry へジャンプ
       │
       └─ [start.S]     通常の p-kernel 起動シーケンスへ (長モード移行等)
```

### なぜこれが「方舟」に重要か

- カーネルが壊れても kloader (8 KB) が生きていれば再起動できる
- Phase 16b 完成後: 別ノードからカーネルを UDP 受信して即時ブート
  → ディスク障害 + カーネル破損でも、ネットワーク上の仲間がいれば復活する

---

## 必要な環境

```sh
sudo apt install -y qemu-system-x86 gcc-i686-linux-gnu binutils-i686-linux-gnu
```

## GDB デバッグ

```sh
# ターミナル 1
make debug

# ターミナル 2
i686-linux-gnu-gdb bootloader.bin
(gdb) target remote :1234
(gdb) break usermain
(gdb) continue
```
