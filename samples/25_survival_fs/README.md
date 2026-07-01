# 25_survival_fs — ARK: the soul test

ARK（`arch/common/arkfs.c`）は、p-kernel の **滅びない図書館** にふさわしい
ローカル永続ファイルシステムである。FAT32 と違い：

- **内容アドレス**：block-id = `sha256(bytes)`。重複排除と自己検証が無料。
- **追記専用ログ + アトミックコミット**：既存バイトを決して上書きしない。
  電源喪失（torn write）は**ログ末尾しか壊せない**。
- **版を刻む**：上書きしても旧版はログに残り、`ark_read_version` で読める。

設計の全体は [`docs/architecture/survival-fs.md`](../../docs/architecture/30-module/survival-fs.md)。

## 実行

```sh
./run.sh        # 失敗時は exit 非0
```

`run.sh` は `arch/common/arkfs.c` + `relay/sha256.c` を `ARK_HOST_TEST` で
ホストビルドし、**ファイル裏のブロックデバイス**に対して以下を検証する：

1. **(0) RAM 自己テスト** — ロジックレベル（`ark_self_test`）。
2. **(a) CRUD + 版管理** — v1→v2→v3 と書き、**旧版 v1/v2 が `readv` で復元できる**
   （図書館）。`readdir` が子を列挙する。
3. **(c) 内容アドレス** — 同一内容を2ファイルに書いても **block は1つ**（dedup）。
   ディスク上の1バイトを反転すると **読み出しで `CORRUPT` を検出**（fsck 不要）。
4. **(b) クラッシュ整合性（魂のテスト）** — `v1` を durable にしてから、`v2` 書き込みの
   **全デバイス書き込み点**で**本物の SIGKILL**（電源喪失をデバイス層で注入）を打ち、
   **別プロセスで再マウント**して読む。結果は常に
   **「v1 が完全」か「v2 が完全」**のどちらか — 半端な破損状態は決して生じない。

注入は2モード：
- `ARK_KILL_BEFORE=N` … N番目のデバイス書き込みの**直前**に SIGKILL（届かなかった書き込み）。
- `ARK_KILL_TORN=N` … N番目のセクタを**半分だけ書いて残り半分はゴミ**にしてから SIGKILL
  （古典的な torn sector）。

## 期待出力（抜粋）

```
  kill=TORN   #4  writer_rc=137  remount: rolled back to v1 (intact)
  kill=TORN   #5  writer_rc=137  remount: committed v2 fully (intact)
  PASS  every crash point left a clean, fully-consistent store
  ...
  [inject] torn half-sector then SIGKILL at write #4 (lba 11)
  remount: READ: GOOD-V1 / VERSION: 1
  PASS  torn commit rolled back to GOOD-V1
ALL PASS — ARK is content-addressed, versioned, and crash-safe.
```

`writer_rc=137` は writer が SIGKILL（128+9）で**実際に殺された**証拠。
その直後の別プロセス再マウントが無傷であることが、アトミックコミットの論証
（`survival-fs.md §5`）の実証になっている。

## ハーネスのサブコマンド（`arkfs_test`）

```
format  <img> <nsectors>      # 新規 ARK イメージを作る
write   <img> <path> <string> # 1コミットで書く（ARK_KILL_* で注入）
read    <img> <path>          # 現在版を読む（CORRUPT で exit 3）
version <img> <path>          # 現在の版番号
readv   <img> <path> <ver>    # 過去版を読む（図書館）
history <img> <path>          # 版の履歴
ls      <img> <dir>           # readdir
blocks  <img>                 # 格納 block 数（dedup の証拠）
corrupt <img> <byteoffset>    # 1バイト反転（rot 注入）
selftest                      # RAM 自己テスト
```

バイナリ・イメージはコミットしない（`mktemp` 上で動き、終了時に消える）。
