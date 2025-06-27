# QEMU x86 ブートローダー & デバッグ環境

このディレクトリには、QEMU仮想環境でx86アーキテクチャ向けのp-kernelを起動するためのブートローダーと、包括的なデバッグ環境が含まれます。

## 主要ファイル

- `main.c`: メインのブートローダーコード（Cレベル機能）
- `start.S`: アセンブリレベルのスタートアップルーチン（16ビットリアルモード）
- `linker.ld`: メモリレイアウトを定義するリンカースクリプト
- `Makefile`: ビルドとデバッグ用のターゲット定義
- `debug.md`: デバッグ環境の使用方法詳細

## クイックスタート

```sh
# ビルド
make

# 通常実行（シリアル出力をコンソールに表示）
make run

# デバッグモード（GDB接続可能）
make debug
```

## デバッグ環境の機能

### 利用可能なMakeターゲット

| コマンド | 機能 | 用途 |
|---------|------|------|
| `make` | ブートローダーをビルド | 基本的なコンパイル |
| `make run` | QEMUで実行、シリアル出力をコンソールに表示 | 通常のテスト実行 |
| `make debug` | GDB接続可能なデバッグモードでQEMU実行 | ステップデバッグ |
| `make run-log` | シリアル出力をファイル（serial.log）に保存 | ログ分析 |
| `make clean` | ビルド生成物を削除 | クリーンビルド |

### シリアル出力デバッグ

このブートローダーは包括的なシリアル出力機能を提供します：

1. **アセンブリレベル出力**: `start.S`で直接シリアルポート（COM1: 0x3F8）を制御
2. **Cレベル出力**: `main.c`内のserial_init()とprint()関数でより高レベルな出力
3. **リアルタイム確認**: `make run`でコンソールに即座に表示
4. **ログ保存**: `make run-log`で`serial.log`ファイルに保存

### GDBデバッグ

GDBデバッグ機能を提供していますが、利用には事前準備が必要です：

```sh
# 事前準備: GDBインストール
sudo apt install gdb

# ターミナル1: デバッグモードでQEMU起動
make debug

# ターミナル2: GDBでリモート接続
gdb
(gdb) target remote :1234
(gdb) set architecture i8086
(gdb) info registers
(gdb) continue
```

**重要**: この環境ではGDBが未インストールの場合があります。READMEの依存関係セクションを参照してください。

### 実際の出力例

```
Hello from Boot!
Calling C main...
```

**注意**: 現在の実装では、アセンブリレベルでのシリアル出力は完全に動作していますが、Cレベルのmain関数実行は16ビット環境での制約により制限されています。

## 依存関係と環境構築

### 必要なパッケージ

- QEMU (i386エミュレーション) - **必須**
- GNU Binutils - **必須**
- GCC (標準版、16ビットコード生成対応) - **必須**
- GDB - **GDBデバッグ機能用（オプション）**

### Ubuntu/Debianでのインストール方法

```sh
# 基本パッケージ（必須）
sudo apt update
sudo apt install -y qemu-system-x86 build-essential
sudo apt install -y binutils gcc

# GDBデバッグ用（オプション）
sudo apt install -y gdb
```

### 動作確認済み環境

- Ubuntu 24.04 LTS (Noble)
- QEMU 8.2.2
- GCC 13.x
- GNU Binutils 2.x

### ビルドと実行の流れ

1. **ビルド**:
```sh
cd p-kernel/boot/x86
make
```

2. **シリアル出力テスト**:
```sh
make run
# または
make run-log && cat serial.log
```

3. **デバッグセッション**:
```sh
# ターミナル1
make debug

# ターミナル2 
gdb
(gdb) target remote :1234
(gdb) break _start
(gdb) continue
(gdb) step
```

## 制限事項

### 現在の実装の制限

1. **Cレベル関数実行**: 16ビットリアルモードでのC関数実行に制約があり、main関数の完全実行は限定的
2. **メモリ制約**: 512バイトのブートセクタ制限による機能制約
3. **プロテクトモード未対応**: 現在は16ビットリアルモードのみ対応
4. **GDB依存**: GDBデバッグ機能は別途インストールが必要

### 動作確認済み機能

✅ アセンブリレベルシリアル出力  
✅ QEMUでの実行とログ保存  
✅ Makefileベースのビルドシステム  
✅ タイムアウト機能による安全な実行  

## トラブルシューティング

### シリアル出力が表示されない

**症状**: `make run`でメッセージが表示されない  
**解決策**:
- 最初に`make clean && make`でクリーンビルド
- `timeout 3 make run`でタイムアウト付きテスト
- `make run-log && cat serial.log`でファイル出力確認

### QEMUがブロッキングされる

**症状**: QEMUが終了しない  
**解決策**:
- Ctrl+Cで強制終了
- `pkill -f qemu-system-i386`でプロセス終了
- 常に`timeout`コマンドを使用

### GDB接続できない

**症状**: `gdb: command not found`または接続エラー  
**解決策**:
- `sudo apt install gdb`でGDBをインストール
- `make debug`でQEMUがデバッグモード起動を確認
- ポート1234の競合確認: `lsof -i :1234`

### ビルドエラー

**症状**: アセンブリまたはリンクエラー  
**解決策**:
- 依存パッケージ確認: `gcc --version && as --version && ld --version`
- `make clean`でクリーンビルド
- Makefileの設定確認

## 技術詳細

### メモリレイアウト

- ロード先アドレス: 0x7C00（標準MBRブートセクタ位置）
- スタック: 0x7C00より下位に設定
- ブートシグネチャ: 0x55AA（セクタ末尾510-511バイト）

### シリアルポート設定

- ポート: COM1 (0x3F8)
- ボーレート: 38400bps（分周器=3）
- データ形式: 8ビット、パリティなし、ストップビット1
- FIFO: 有効

### 実際の実行フロー

1. BIOS → `_start` (start.S:4)
2. シリアルポート初期化（アセンブリ）(start.S:11-35)
3. "Hello from Boot!" 出力 (start.S:38-45)
4. "Calling C main..." 出力 (start.S:49-56)
5. `main()` 関数呼び出し試行 (start.S:60)
6. **制限**: C関数の完全実行は16ビット環境制約により限定的
7. 無限ループ (start.S:62-63)

### ファイル構成と役割

- `start.S`: 16ビットアセンブリブートコード（実際の実行部分）
- `main.c`: Cレベル関数定義（制限付き実行）
- `linker.ld`: メモリレイアウト定義
- `Makefile`: ビルドとデバッグターゲット定義
- `serial.log`: シリアル出力ログファイル（実行時生成）
