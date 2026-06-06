# 14_genome — §3 自己再生: 群れが新しい装甲板を一個の細胞に育てる

survival-network.md §3 のデモ。**生存している細胞（node 1）が自分の
genome（重み＋コード＋役割）を p-fs に蒔き、完全に空の装甲板（node 2）
がそれだけから一個のフル細胞に発芽する。**

```
./sprout.sh
```

## 何が起きるか

| t | node 1（フル細胞） | node 2（空の装甲板） |
|---|---|---|
| 0s | — | 起動。`PKERNEL_SPROUT=1` — DNA を待つ |
| 8s | `dtr train` — 本物の SGD、95%/100% | 待っている |
| 14s | `dtr save` — 重み → p-fs `dtr/weights` | 待っている |
| 16s | `selfc save genome.c` — C ソース → p-fs | 待っている |
| 18s | `genome publish cell` — マニフェスト → p-fs `genome/manifest` | 待っている |
| ~25s | （gossip が運ぶ） | マニフェスト到着 → 重み復元 → コードを**体内コンパイル**して実行 → `sprouted` |
| 55s | — | `dtr eval` → **node 1 と同一の 95%/100%**（1 step も学習していない） |

## PASS 条件（全部、node 2 のログで）

1. `[genome] manifest arrived` — DNA が gossip で届いた
2. `[guard] dtr recover: weights restored from p-fs` — 脳の復元
3. `dtr eval` の 2 行が node 1 の学習後 eval と**文字単位で一致**
   （float32 blob はビット同一 → 数字も同一）
4. `I was compiled at runtime inside the kernel` — node 1 の C コードが
   node 2 のカーネル内部で実行された（libtcc、ファイル無し）
5. `[genome] sprouted: a full cell grew from the swarm's DNA`

どれか欠ければ exit 非 0。libtcc の無いビルドでは 4 が証明できないので
スクリプトは正直に最初から FAIL する（`apt-get install libtcc-dev`）。

## 仕組み

新しい臓器は無い。`arch/common/genome.c` はオーケストレーションだけ:

- 重み = `dtr/weights`（content-addressed、P1/P2 で region 自動複製）
- コード = `genome.c`（selfc が体内コンパイル）
- マニフェスト = 固定幅 124B 以下の `GENOME_MANIFEST`（これも普通の
  p-fs ブロックとして複製される）

正直な限界は `docs/architecture/genome.md` §5。

ログ: `/tmp/genome_node{1,2}.log` `/tmp/genome_relay.log`
