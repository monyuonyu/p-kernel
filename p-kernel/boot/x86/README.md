# QEMU x86 32ビットカーネル & デバッグ環境

このディレクトリには、QEMU仮想環境でx86アーキテクチャ向けの32ビットp-kernelと、包括的なデバッグ環境が含まれます。Multiboot準拠の32ビットプロテクトモードカーネルとして動作します。

## 主要ファイル

- `main.c`: 32ビットカーネルのメインコード（C言語）
- `start.S`: Multibootヘッダーと32ビットアセンブリ初期化コード
- `linker.ld`: ELF32形式用メモリレイアウト定義
- `Makefile`: ビルドとデバッグ用のターゲット定義

## クイックスタート

```sh
# ビルド
make

# 32ビットカーネル実行（シリアル出力をコンソールに表示）
make run

# デバッグモード（GDB接続可能）
make debug

# ブートディスク形式での実行
make run-boot
```

## デバッグ環境の機能

### 利用可能なMakeターゲット

| コマンド | 機能 | 用途 |
|---------|------|------|
| `make` | 32ビットカーネルをビルド（ELF形式） | 基本的なコンパイル |
| `make run` | QEMUで32ビットカーネル実行、シリアル出力をコンソールに表示 | 通常のテスト実行 |
| `make debug` | GDB接続可能なデバッグモードでQEMU実行 | ステップデバッグ |
| `make run-log` | シリアル出力をファイル（serial.log）に保存 | ログ分析 |
| `make run-boot` | ブートディスク形式での実行 | レガシー互換テスト |
| `make clean` | ビルド生成物を削除 | クリーンビルド |

### 32ビットカーネル機能

この32ビットカーネルは以下の機能を提供します：

1. **Multiboot準拠**: 標準的なMultibootヘッダーでQEMU `-kernel`オプション対応
2. **32ビットプロテクトモード**: 完全な32ビット環境での実行
3. **シリアル出力**: 32ビットモードでの包括的なシリアル通信機能
4. **メモリ管理**: 32ビットメモリ空間への直接アクセス
5. **C言語ランタイム**: GCC生成の32ビットCコードの完全実行

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
32-bit kernel started!
32-bit C kernel started!
Protected mode C function executing successfully!
32-bit arithmetic test: PASSED!
32-bit memory access test: PASSED!
32-bit protected mode kernel initialization complete!
```

**機能確認**: 32ビットプロテクトモードでの完全なC言語実行、算術演算、メモリアクセスが全て正常に動作しています。

## 依存関係と環境構築

### 必要なパッケージ

- QEMU (i386エミュレーション) - **必須**
- GNU Binutils - **必須**
- GCC (標準版、32ビットコード生成対応) - **必須**
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

## 機能状況

### 実装済み機能

✅ **32ビットプロテクトモード**: 完全な32ビット環境  
✅ **Multiboot準拠カーネル**: 標準的なカーネル形式  
✅ **32ビットC言語実行**: GCC生成コードの完全実行  
✅ **32ビットシリアル出力**: プロテクトモードでの通信機能  
✅ **32ビット算術演算**: 正常な整数演算  
✅ **32ビットメモリアクセス**: メモリ空間への読み書き  
✅ **QEMUデバッグ環境**: 包括的なデバッグ機能  
✅ **ELF形式出力**: 標準的なバイナリ形式  

### 制限事項

1. **GDB依存**: GDBデバッグ機能は別途インストールが必要
2. **シングルコア**: 現在はシングルプロセッサ環境のみ対応
3. **基本メモリ管理**: 高度なメモリ管理機能は未実装

## トラブルシューティング

### カーネルが起動しない

**症状**: `make run`でカーネルが起動しない  
**解決策**:
- 最初に`make clean && make`でクリーンビルド
- `file bootloader.bin`でELF32形式を確認
- `timeout 5 make run`でタイムアウト付きテスト
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

- カーネルロード先: 0x100000（1MB）- Multiboot標準
- スタック: 0x200000（2MB）
- 形式: ELF32実行形式
- アーキテクチャ: Intel 80386 (32ビット)

### シリアルポート設定

- ポート: COM1 (0x3F8)
- ボーレート: 38400bps（分周器=3）
- データ形式: 8ビット、パリティなし、ストップビット1
- FIFO: 有効

### 実際の実行フロー

1. QEMU Multibootローダー → `_start` (start.S:17)
2. 32ビットプロテクトモード環境設定 (start.S:18-21)
3. 32ビットシリアルポート初期化 (start.S:24-56)
4. "32-bit kernel started!" 出力 (start.S:13-15)
5. 32ビット`main()` 関数呼び出し (start.S:18)
6. C言語カーネル初期化実行 (main.c:39-66)
7. 32ビット機能テスト実行（算術・メモリアクセス）
8. 無限ループ (main.c:66)

### ファイル構成と役割

- `start.S`: Multibootヘッダーと32ビットアセンブリ初期化コード
- `main.c`: 32ビットCカーネルのメイン実装
- `linker.ld`: ELF32形式用メモリレイアウト定義
- `Makefile`: ビルドとデバッグターゲット定義（32ビット対応）
- `serial.log`: シリアル出力ログファイル（実行時生成）
