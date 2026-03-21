# ステップ9: ディレクトリ一覧 (readdir)

`sys_readdir()` を使ってディレクトリのエントリ一覧を取得するサンプルです。

## 学べること

- **`sys_readdir(path, buf, max)`** — 指定パスのエントリを `PK_SYS_DIRENT` 配列に取得する
- **`PK_SYS_DIRENT`** — `name` / `size` / `is_dir` フィールドを持つエントリ構造体
- ファイルとディレクトリを区別して表示する方法

## ビルドと実行

```bash
cd userland/x86
make 09_readdir/readdir.elf

cd boot/x86
make disk && make run-disk
p-kernel> exec readdir.elf
```

## 期待する出力

```
========================================
 readdir: ディレクトリ一覧デモ
========================================

--- / (root) ---
  type  size       name
  ----  ---------  ----------------------------------------
  [fil]    34964  B  hello.elf
  [fil]    36956  B  posix_io.elf
  [fil]    37564  B  rtos_task.elf
  [fil]    37800  B  rtos_sync.elf
  [fil]    41020  B  all_demo.elf
  [fil]    37780  B  net_infer.elf
  [fil]    35180  B  stdin_echo.elf
  [fil]    35396  B  http_get.elf
  [fil]    35180  B  readdir.elf
  ----  ---------  ----------------------------------------
  9 entries (9 files, 0 dirs)   total: 371 KB

========================================
 readdir: done
========================================
[proc] exited (code=0)
```

## syscall 仕様

| パラメータ | 説明 |
|-----------|------|
| `path` | 一覧取得するディレクトリのパス (`"/"` = ルート) |
| `buf` | `PK_SYS_DIRENT` 配列へのポインタ |
| `max` | バッファの最大エントリ数 (最大 32) |
| 戻り値 | 実際のエントリ数、エラー時は -1 |
