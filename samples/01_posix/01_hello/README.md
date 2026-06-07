# ステップ1: Hello World

p-kernel で動く最初のユーザープログラムです。

## 学べること

- ring-3（ユーザー空間）と ring-0（カーネル空間）の違い
- `INT 0x80` による**システムコール**の仕組み
- `_start()` — libc を使わない ELF のエントリーポイント
- `plibc_puts()` / `plib_puti()` による文字列・数値出力

## ソースファイル

```
01_hello/
└── hello.c    メインプログラム
```

## ビルドと実行

```bash
# ビルド
cd boot/x86/user_hello
make 01_hello/hello.elf

# QEMU で実行
p-kernel> exec hello.elf
```

## 期待する出力

```
Hello, p-kernel!

このプログラムは ring-3 (ユーザー空間) で動いています。
INT 0x80 でカーネルにシステムコールを発行しています。

簡単な計算: 6 x 7 = 42
カウントダウン: 3... 2... 1... 完了!

次のサンプル: exec posix_io.elf
```

## コードのポイント

### `_start()` がエントリーポイント

通常の C プログラムは `main()` から始まりますが、libc を使わない
ベアメタル ELF では **`_start()`** が直接呼ばれます。
リンカスクリプト `user.ld` に `ENTRY(_start)` と記述されています。

### `INT 0x80` でカーネルを呼ぶ

`plib_puts()` → `sys_write()` → `__sc(4, ...)` → `int $0x80`

```c
static inline int __sc(int nr, int a0, int a1, int a2)
{
    int ret;
    asm volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(nr), "b"(a0), "c"(a1), "d"(a2)
                 : "memory");
    return ret;
}
```

`eax` にシステムコール番号、`ebx/ecx/edx` に引数を渡します。

### ロードアドレス

ユーザー ELF は `0x00400000` にロードされます（`user.ld` で定義）。
カーネルは `0x00000000〜0x003FFFFF` に存在し、ユーザーからは直接アクセスできません。

## 次のステップ

```
p-kernel> exec posix_io.elf   ← FAT32 ファイルI/O
```
