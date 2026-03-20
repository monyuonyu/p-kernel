# boot/x86 — p-kernel x86/QEMU 統合ビルド

micro T-Kernel 2.0 + 完全 TCP/IP スタック + **AI カーネルプリミティブ** +
**分散 RPC（DRPC）** を含む x86/QEMU 向けシングルバイナリのビルドディレクトリです。

## クイックスタート

```sh
cd p-kernel/boot/x86

# ビルド
make

# シングルノード実行（VGA + シリアル stdio + RTL8139 NIC）
make run

# ヘッドレス実行
make run-headless

# 分散 2 ノード（別ターミナルで両方起動）
make run-node0   # 端末 1: node 0  IP=10.1.0.1
make run-node1   # 端末 2: node 1  IP=10.1.0.2

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
                    DRPC ハートビートタスク起動（priority 5）
                    ネットスタック起動（ARP 事前送信）
                    ネット RX タスク起動（priority 3）
```

## ビルド構成

Makefile は以下の 4 つのソースグループを統合してリンクします。

| グループ | ソースパス | 内容 |
|---------|---------|------|
| boot/x86 | `./` | start.S、main.c、idt.c、memory.c、pic.c、timer.c、isr.S |
| arch/x86 | `../../arch/x86/` | CPU 初期化、シリアル、VGA、キーボード、シェル、PCI、RTL8139、netstack、drpc、**tensor、ai_job、pipeline、fedlearn、ai_stats** |
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
| `make run-node0` | 分散モード node 0（UDP マルチキャスト） |
| `make run-node1` | 分散モード node 1（UDP マルチキャスト） |
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

### 分散 2 ノード（UDP マルチキャスト）

```
Node 0: MAC=52:54:00:00:00:01  IP=10.1.0.1
Node 1: MAC=52:54:00:00:00:02  IP=10.1.0.2
NW    : socket mcast=230.0.0.1:1234（仮想 Ethernet ハブ）
DRPC  : UDP port 7374
```

同一バイナリを MAC アドレスで自動識別 — 再コンパイル不要。

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
