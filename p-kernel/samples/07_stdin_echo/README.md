# ステップ7: stdin エコー

標準入力から 1 行ずつ読み込み、そのまま標準出力へ返すシンプルなサンプルです。
空行を入力するとプログラムが終了します。

## 学べること

- **`sys_read(fd=0, buf, len)`** — 標準入力から読み込む（ブロッキング）
- **`sys_write(fd=1, buf, len)`** — 標準出力への書き込み
- **シェルの stdin リレー** — `exec` 中はシェルがシリアル入力を ELF の stdin へ転送する仕組み

## ビルドと実行

```bash
cd userland/x86
make 07_stdin_echo/stdin_echo.elf

cd boot/x86
make disk && make run-disk
p-kernel> exec stdin_echo.elf
```

## 期待する出力

```
========================================
 stdin_echo: 標準入力エコーデモ
========================================
  空行を入力すると終了します。

echo> hello
hello
echo> world
world
echo>
========================================
 stdin_echo: done
========================================
[proc] exited (code=0)
```
