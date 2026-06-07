# ステップ2: POSIX ファイル I/O

p-kernel 内蔵の FAT32 ファイルシステムを POSIX 互換 API で操作するサンプルです。

## 学べること

- `sys_open` / `sys_write` / `sys_read` / `sys_close` によるファイル読み書き
- `sys_lseek` によるシーク操作（SEEK_SET / SEEK_END）
- `sys_mkdir` / `sys_rename` / `sys_unlink` によるファイル・ディレクトリ操作
- ファイルディスクリプタ（fd）の仕組み

## ソースファイル

```
02_posix_io/
└── posix_io.c    メインプログラム（3 つのデモ関数）
```

## ビルドと実行

```bash
# 1) サンプル ELF をビルド
cd userland/x86
make 01_posix/02_posix_io/posix_io.elf   # または make all で全サンプル

# 2) ディスクイメージに格納して QEMU 起動
cd boot/x86
make disk && make run-disk

# 3) シェルから実行
p-kernel> exec posix_io.elf
```

## 期待する出力

```
=== POSIX ファイル I/O サンプル ===

--- [1] ファイル書き込み / 読み込み ---
  ファイル作成: /sample.txt (fd=3)
  書き込みバイト数: 30
  ファイルをクローズしました
  読み込み内容: "p-kernel から書き込みました。"
  /sample.txt を削除しました

--- [2] lseek によるシーク操作 ---
  SEEK_SET(3) から 3 バイト読み: "DEF"
  SEEK_END 後の read: 0 バイト (0 = EOF)

--- [3] ディレクトリ作成 / ファイル名変更 ---
  /mydir を作成しました
  /mydir/old.txt を作成しました
  old.txt → new.txt にリネームしました
  /mydir/old.txt は存在しません (正常)
  /mydir/new.txt は存在します (正常)
  クリーンアップ完了

=== 完了 ===
次のサンプル: exec rtos_task.elf
```

## ファイルディスクリプタの仕組み

| fd | 意味 |
|----|------|
| 0  | 標準入力（`exec` 中はシェルがシリアル入力を中継。03_stdin_echo 参照）|
| 1  | 標準出力 → シリアルポート |
| 2  | 標準エラー → シリアルポート |
| 3〜| ファイル（FAT32 ディスク上）|

`sys_open()` が返す fd は必ず **3 以上**になります。

## open フラグ

| フラグ | 意味 |
|--------|------|
| `O_RDONLY` | 読み込み専用 |
| `O_WRONLY` | 書き込み専用 |
| `O_CREAT`  | ファイルが無ければ作成 |
| `O_RDWR`   | 読み書き両用 |

## lseek whence

| 定数 | 意味 |
|------|------|
| `SEEK_SET` | ファイル先頭からのオフセット |
| `SEEK_CUR` | 現在位置からのオフセット |
| `SEEK_END` | ファイル末尾からのオフセット |

## 次のステップ

```
p-kernel> exec rtos_task.elf   ← RTOS タスク管理
```
