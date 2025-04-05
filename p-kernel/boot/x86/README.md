# QEMU x64 ブートローダー

このディレクトリには、QEMU仮想環境でx86_64アーキテクチャ向けのp-kernelを起動するためのブートローダーが含まれます。

## 主要ファイル

- `main.c`: メインのブートローダーコード
- `start.S`: アセンブリレベルのスタートアップルーチン
- `linker.ld`: メモリレイアウトを定義するリンカースクリプト

## ビルド方法

```sh
make
qemu-system-x86_64 -kernel p-kernel.bin
```

## 依存関係と環境構築

### 必要なパッケージ

- QEMU (x86_64エミュレーション)
- GNU Binutils
- GCCクロスコンパイラ (x86_64-elf-)

### Ubuntu/Debianでのインストール方法

```sh
sudo apt update
sudo apt install -y qemu-system-x86 build-essential
sudo apt install -y gcc-x86-64-elf binutils-x86-64-elf
```

### ビルド手順

1. ツールチェインの確認:
```sh
x86_64-elf-gcc --version
```

2. ブートローダーのビルド:
```sh
cd p-kernel/boot/x86
make
```

3. QEMUでの実行:
```sh
qemu-system-x86_64 -kernel bootloader.bin -serial stdio
```
