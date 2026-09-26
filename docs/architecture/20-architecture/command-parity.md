# コマンドの横並び（command parity）— 段階0の設計メモ

inbox #4（2026-09-26、翔太「プラットフォームでコマンドが多い・少ないの差が出るのは嫌」）。
段階0は**見える化だけ**。カーネルの挙動も crown も変えない。

## 何を作るか

- `tools/cmdmatrix/gen.py`: 6つの対象のシェルのソースを読み、コマンド名 × 対象の表を
  `docs/architecture/command-matrix.md` に出す（あり／なし／不可＋理由）
- CI: `gen.py --check` が「生成し直した表」と「コミットされた表」を比べ、食い違えば赤
- 「不可」とその理由は手で書く小さな台帳 `tools/cmdmatrix/exceptions.txt`（1行1件、理由は1行）

## 読み方（2026-09-27 に決めた）

実行して `help` を集める方式は採らない。ベアメタルは QEMU が要り、`help` が実物と
合っている保証も無い（合っていないこと自体が #4 の問題）。**ソースを静的に読む。**
いまの振り分けの書き方は3種類だけなので、それぞれに1つずつ読み手を書く:

| 対象 | ソース | 書き方 |
|---|---|---|
| Linux 版 x86_64 / aarch64（Android も同じ中身） | `arch/linux/*/usermain.c` | `starts_with(line, n, "word")` |
| ベアメタル AArch64 | `arch/aarch64/usermain.c` | `strneq(line, "word", n)` |
| ベアメタル x86 | `arch/x86/shell.c` | `cmd[0]=='w' && cmd[1]=='o' && …` の連なり（42か所）を単語に戻す |
| Windows x86_64 | `arch/linux/x86_64/usermain.c`（Makefile の ARCH_SHARED_X86_SRCS。usermain.c に Windows 用の `#ifdef` は無い） | Linux 版と同じ |

09-26 に数えた「Windows はごくわずか」は、ソースの上では当たらない（Linux x86_64 と
同じ 52）。実行すると stub で空振りするもの（selfc など）があるはずで、それは段階2で
実行して確かめ、表に「不可」か「空振り」として書く。

読み手が拾えなかった振り分け（別の書き方）が残ると表が嘘になるので、各ソースで
「コマンドを比べていそうな行」の数と「拾えた数」を並べて出し、差があれば赤にする。

## 限界（正直に）

- 静的に読むので、前方一致（`strneq(line,"ai",2)` は `air` にも当たる）や、
  サブコマンド（`mind teach`）は第1語だけを表にする。サブコマンドの横並びは段階1で
- Android は Linux aarch64 と同じ `.so` なので同じ列にし、表にそう書く
- 段階1（共通の台帳）で1文字比べが消えれば、読み手は台帳を読むだけになる
