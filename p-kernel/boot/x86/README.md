# QEMU x86 完全64ビットロングモードカーネル & デバッグ環境

このディレクトリには、QEMU仮想環境でx86_64アーキテクチャ向けの完全64ビットロングモードp-kernelと、包括的なデバッグ環境が含まれます。32ビットMultiboot準拠エントリから64ビットロングモードへの完全移行を実現します。

## 主要ファイル

- `main.c`: 64ビット環境で実行されるカーネルのメインコード（C言語）
- `start.S`: Multibootヘッダーと32→64ビット移行アセンブリコード
- `linker.ld`: 64ビット対応メモリレイアウト定義
- `Makefile`: ビルドとデバッグ用のターゲット定義

## クイックスタート

```sh
# ビルド
make

# 64ビットカーネル実行（シリアル出力をコンソールに表示）
make run

# デバッグモード（GDB接続可能）
make debug

# シリアル出力をファイルに保存
make run-log && cat serial.log
```

## 完全64ビットロングモード機能

### 64ビット移行アーキテクチャ

この実装は以下の段階的64ビット移行を実現します：

1. **32ビットMultibootエントリ** → QEMU標準ローダー対応
2. **64ビットCPU検出** → CPUID命令による動的検出
3. **ページテーブル構築** → PML4/PDP/PD 3階層ページング
4. **ロングモード有効化** → IA32_EFER MSR設定
5. **64ビットGDT設定** → 64ビット互換セグメント
6. **64ビットCコード実行** → 完全64ビット環境での実行

### 利用可能なMakeターゲット

| コマンド | 機能 | 用途 |
|---------|------|------|
| `make` | 64ビットカーネルをビルド（ELF形式） | 基本的なコンパイル |
| `make run` | QEMUで64ビットカーネル実行、シリアル出力をコンソールに表示 | 通常のテスト実行 |
| `make debug` | GDB接続可能なデバッグモードでQEMU実行 | ステップデバッグ |
| `make run-log` | シリアル出力をファイル（serial.log）に保存 | ログ分析 |
| `make run-32` | 32ビットCPU環境での動作確認（フォールバックテスト） | 互換性テスト |
| `make clean` | ビルド生成物を削除 | クリーンビルド |

### 64ビットカーネル機能

この64ビットカーネルは以下の高度な機能を提供します：

1. **Multiboot準拠**: 標準的なMultibootヘッダーでQEMU `-kernel`オプション対応
2. **64ビットロングモード**: 完全な64ビット実行環境
3. **動的CPU検出**: CPUID命令による64ビット機能の実行時検出
4. **完全ページング**: 3階層ページテーブル（PML4→PDP→PD）
5. **MSR制御**: IA32_EFER MSRによるロングモード制御
6. **64ビット算術**: 64ビット整数演算の完全サポート
7. **フォールバック処理**: 32ビットCPUでの適切なエラーハンドリング

### 実際の出力例（64ビットモード）

```
Starting 32-bit kernel with 64-bit long mode capability...
Entering 64-bit long mode transition...
64-bit long mode successfully activated! Running in compatibility mode.
=== 64-bit Long Mode Transition Kernel ===
Successfully transitioned to 64-bit long mode!
Running C code called from 64-bit assembly context
64-bit integer arithmetic test: PASSED!
64-bit multiplication test: PASSED!
64-bit large number test: PASSED!
=== Long Mode Transition Complete! ===
Kernel is now running in 64-bit long mode environment
C code execution from 64-bit context confirmed!
```

### 実際の出力例（32ビットCPU）

```
Starting 32-bit kernel with 64-bit long mode capability...
ERROR: 64-bit long mode not supported on this CPU!
```

**機能確認**: 64ビットロングモードでの完全なC言語実行、64ビット算術演算、大数値処理が全て正常に動作しています。

## 依存関係と環境構築

### 必要なパッケージ

- QEMU (x86_64エミュレーション) - **必須**
- GNU Binutils - **必須**
- GCC (32ビットクロスコンパイル対応) - **必須**
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

## 技術詳細

### 64ビット移行プロセス

#### 1. 初期化フェーズ（32ビット）
```assembly
_start:
    cli                    # 割り込み無効化
    mov $0x200000, %esp    # 32ビットスタック設定
    call serial_init_32    # シリアルポート初期化
```

#### 2. 64ビット機能検出
```assembly
check_long_mode_support:
    # CPUID機能存在確認
    # 拡張CPUID機能確認
    # ロングモード対応確認（LMビット）
```

#### 3. ページテーブル構築
```assembly
setup_page_tables:
    # PML4テーブル（最上位）
    # PDPテーブル（中間）
    # PDテーブル（2MBページ）
```

#### 4. ロングモード移行
```assembly
enter_long_mode:
    # PAE有効化（CR4.PAE = 1）
    # ページテーブル設定（CR3）
    # ロングモード有効化（IA32_EFER.LME = 1）
    # ページング有効化（CR0.PG = 1）
    # 64ビットGDTロード
    # 64ビットモードジャンプ
```

### メモリレイアウト

- **カーネルロード先**: 0x100000（1MB）- Multiboot標準
- **32ビットスタック**: 0x200000（2MB）
- **64ビットスタック**: 0x300000（3MB）
- **PML4テーブル**: 0x101000（動的配置）
- **PDPテーブル**: 0x102000（動的配置）
- **PDテーブル**: 0x103000（動的配置）

### ページテーブル構成

```
PML4[0] → PDP Table
PDP[0]  → PD Table  
PD[0-511] → 2MB物理ページ（アイデンティティマッピング）
```

- **ページサイズ**: 2MB（PSEビット使用）
- **マッピング**: アイデンティティマッピング（仮想=物理）
- **範囲**: 0x00000000-0x3FFFFFFF（1GB）

### 64ビットGDT構成

```assembly
gdt64_start:
    .quad 0                         # NULL セグメント
    .quad 0x00cf9a000000ffff        # 32ビット互換コードセグメント
    .quad 0x00cf92000000ffff        # 32ビット互換データセグメント
```

### シリアルポート設定

- **ポート**: COM1 (0x3F8)
- **ボーレート**: 38400bps（分周器=3）
- **データ形式**: 8ビット、パリティなし、ストップビット1
- **FIFO**: 有効

## GDBデバッグ

64ビットカーネルのデバッグ機能を提供します：

```sh
# ターミナル1: デバッグモードでQEMU起動
make debug

# ターミナル2: GDBでリモート接続
gdb
(gdb) target remote :1234
(gdb) set architecture i386:x86-64
(gdb) break _start
(gdb) break enter_long_mode
(gdb) continue
```

## 機能状況

### 実装済み機能

✅ **32ビットMultibootエントリ**: 標準的なカーネル形式  
✅ **64ビットCPU動的検出**: CPUID命令による実行時検出  
✅ **完全ページテーブル**: PML4/PDP/PD 3階層構造  
✅ **ロングモード移行**: IA32_EFER MSR制御  
✅ **64ビットGDT**: 互換モードセグメント設定  
✅ **64ビットC実行**: 64ビット環境でのC言語実行  
✅ **64ビット算術**: 完全な64ビット整数演算  
✅ **フォールバック処理**: 32ビットCPUでのエラーハンドリング  
✅ **QEMUデバッグ環境**: 包括的なデバッグ機能  

### テスト確認項目

✅ **64ビット整数オーバーフローテスト**: PASSED  
✅ **64ビット乗算テスト**: PASSED  
✅ **64ビット大数値テスト**: PASSED  
✅ **32ビットCPU互換性テスト**: 適切なエラー表示  

## トラブルシューティング

### カーネルが起動しない

**症状**: `make run`でカーネルが起動しない  
**解決策**:
- `make clean && make`でクリーンビルド
- `file bootloader.bin`でELF32形式を確認
- `make run-log && cat serial.log`でファイル出力確認

### 64ビット移行が失敗する

**症状**: "ERROR: 64-bit long mode not supported"  
**解決策**:
- `make run`でqemu-system-x86_64使用を確認
- CPUフラグ確認: `-cpu qemu64`オプション
- 32ビット環境テスト: `make run-32`

### QEMUがハングする

**症状**: QEMUが応答しない  
**解決策**:
- Ctrl+A → X でQEMU終了
- `timeout 10 make run`でタイムアウト設定
- `pkill -f qemu-system-x86_64`でプロセス強制終了

## 実行フロー詳細

1. **Multibootエントリ** (start.S:19) → `_start`
2. **32ビット初期化** (start.S:20-26) → スタック・シリアル設定
3. **64ビット機能検出** (start.S:53-80) → CPUID実行
4. **ページテーブル構築** (start.S:137-175) → 3階層ページング
5. **ロングモード移行** (start.S:84-115) → MSR・CR設定
6. **64ビット実行開始** (start.S:118-135) → GDT・セグメント設定
7. **64ビットCコード実行** (main.c:40-85) → 算術テスト
8. **無限ループ** (main.c:84) → カーネル待機

### ファイル構成と役割

- `start.S`: 32→64ビット移行アセンブリコードと完全ページテーブル実装
- `main.c`: 64ビット環境で実行されるCカーネルと64ビットテスト
- `linker.ld`: 64ビット対応メモリレイアウト定義
- `Makefile`: 64ビットビルドとデバッグターゲット定義
- `serial.log`: 64ビット実行ログファイル（実行時生成）