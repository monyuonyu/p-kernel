# boot/x86 — p-kernel x86/QEMU 統合ビルド

micro T-Kernel 2.0 + 完全 TCP/IP スタックを含む
x86/QEMU 向けシングルバイナリのビルドディレクトリです。

## クイックスタート

```sh
cd p-kernel/boot/x86

# ビルド
make

# 実行（VGA + シリアル stdio + RTL8139 NIC）
make run

# ヘッドレス実行
make run-headless

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
  └─ [usermain.c]   PS/2 キーボード初期化（IRQ1）
                    シェルタスク起動（priority 2）
                    RTL8139 NIC 初期化（IRQ11）
                    ネットスタック起動（ARP 事前送信）
                    ネット RX タスク起動（priority 3）
```

## ビルド構成

Makefile は以下の 4 つのソースグループを統合してリンクします。

| グループ | ソースパス | 内容 |
|---------|---------|------|
| boot/x86 | `./` | start.S、main.c、idt.c、memory.c、pic.c、timer.c、isr.S |
| arch/x86 | `../../arch/x86/` | CPU 初期化、シリアル、VGA、キーボード、シェル、PCI、RTL8139、netstack |
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

## 動作確認済みの出力

```
=== p-kernel x86 boot ===
[OK]   IDT
[OK]   Memory
[OK]   PIC
[BOOT] Starting T-Kernel...
[OK]  Keyboard (IRQ1)
[OK]  Shell task
[net] RTL8139 ready  I/O=0xC000  IRQ=11  MAC=52:54:00:12:34:56
[arp] Reply: 10.0.2.2 is at 52:55:0A:00:02:02

  +-----------------------------------------+
  |  p-kernel  /  micro T-Kernel 2.0 x86   |
  |  Interactive Shell                      |
  +-----------------------------------------+

p-kernel> http example.com/
DNS: example.com -> 104.18.26.120
[tcp] ESTABLISHED
HTTP/1.1 200 OK
<!doctype html>...<h1>Example Domain</h1>...
--- 1571 bytes ---
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
