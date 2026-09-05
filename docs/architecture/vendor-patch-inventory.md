# ベンダパッチの持ち物リスト（波2 / gap-ledger `VENDOR-PATCH-LOSS`）

作成: 2026-09-03 18:0x の無人 run（指揮者代行）
更新: 2026-09-03 21:xx の無人 run — **陰性コントロール実施済み**、MEM-UPTR の層の同定を訂正
更新: 2026-09-05 の無人 run — **CI 配線を確認**（`26178169`、非BLOCKING）。`gh run view 33812013193`
で新ジョブ「ベンダツリー ローカルパッチ持ち物リスト (非BLOCKING)」が `success` であることを確認した
（陽性コントロールの CI 上での実行はこれで確認済み。陰性コントロールの CI 上での実行は push が要るので未検証のまま）。
更新: 2026-09-06 の無人 run — **層Bの完全列挙を実施（§7）。** 前run群がネットワーク不通で止まっていた
上流 `435096c9` への到達がこの run では通り、`git clone --bare --filter=blob:none` で上流を丸ごと
取得できた。194ファイルの機械比較の結果、真の層Bパッチは **SCHED_RR ラウンドロビン**と
**tstdlib/string.c の LP64 語コピー修正**の2件と判明（詳細は§7）。新アンカー6本を
`check_local_patches.sh` に追加し、4アーム（master陽性/上流純正陰性/`f50c30a0`歴史的陰性/不在自爆）
すべてで実測・確認済み。
対象ツリー: `kernel/mtkernel3/`（上流 `tron-forum/mtkernel_3` の `435096c9` を取り込んだ vendored subtree）
状態: **チェッカは書けて CI にも配線済み（非BLOCKING）。層A・層Bとも主要な「静かに消える」クラスは
アンカー化済み（計19本）。陰性コントロールはホスト上でのみ実証済みで、CI 上では未実証。**

---

## この文書の目的

`VENDOR-PATCH-LOSS` 行が要求しているのは
「**ベンダツリーの差し替えがローカルパッチを落としたことを、9クラスのどれについてでも検出できること**」。
2026-08-12 に作ったゲート `killchurn-x86` は **9クラス中1クラス（L1）しか見ていない**。

この文書はその前段＝**何が載っているかの enumeration（持ち物リスト）**。
リストが無ければ「消えた」ことを検出しようがない。

---

## 1. 発見方法と、その限界（先に正直に書く）

ローカルパッチは **2 つの層**に分かれ、**発見の確実さが違う**。

### 層A: 取り込み**後**に積んだパッチ — `git log` で機械的に完全列挙できる

```
git -C /home/shota/p-kernel log --oneline f50c30a0..master -- kernel/mtkernel3/
```

この run で実行した結果、**3 コミットのみ**:

| コミット | 内容 | ベンダ中核ファイルへの変更 |
|---|---|---|
| `339a66a2` | kill/churn ハードニング L1–L7 の復元 | task.c / task_manage.c / timer.c / timer.h / wait.c / wait.h（+202行） |
| `ff163ac1` | Windows (mingw-w64 PE) ポート P1 | **memory.c / memory.h / mempool.c**（+ windows_x86_64 の新規ディレクトリ群） |
| `440e6d6d` | GCC 14/15 の implicit-decl 修正 | timer.c（+1行） |

**層A は完全**。定義上これ以外に取り込み後のパッチは存在しない。

### 層B: 取り込みコミット `f50c30a0` 自身に**焼き込まれた**パッチ — 完全列挙できていない

VENDOR.md が「LP64 対応・linux_x86_64 ポート・日本語コメント刷新」を加えたと書いている。
これらは `f50c30a0` の中に入っているので、**上の `git log` には出てこない**。

この run で使った発見手段は **コメント中の文字列 `p-kernel` を grep する**というヒューリスティック。
ベンダ中核（`kernel/tkernel/` `kernel/tstdlib/` `include/` `lib/` `config/`）で **50 ファイル**が該当した。
うち上流にも存在する（＝新規追加ではなく**改変**）中核ファイルは:

`memory.h` `memory.c` `mempool.c` `subsystem.h` `subsystem.c` `tstdlib/string.c`
`task_manage.c` `timer.c` `include/tk/syscall.h` `include/sys/machine.h`
`include/sys/inittask.h` `include/compat/*` `config/config.h` ほか

> **限界（潰さないこと）**: これは**ヒューリスティックであって証明ではない**。
> コメントに `p-kernel` と書かなかった改変は**この方法では見つからない**。
> 完全な層B の列挙には上流 `435096c9` との差分が要るが、
> **7月の日本語コメント刷新で差分が巨大**になっており素朴な diff は使えない
> （＝`VENDOR-PATCH-LOSS` 行が既に指摘している問題）。
> **2026-09-06 訂正: 層B の列挙は完了した。** ネットワーク到達性が新たに確認でき、
> 上流をコメント除去した上で機械比較する手法で解決した。**詳細は §7。**

### 層外: SMP の L8/L9 は**このツリーに無い**（この run の確認）

`knl_smp_wake_hook` / `CUR_CTXTSK` は `arch/aarch64/` にある＝**p-kernel 自身のツリー**。
`kernel/mtkernel3/` の差し替えでは**消えない**。
（`task_manage.c:231` にスコープ外である旨のコメントが残っている。）
→ **持ち物リストの守備範囲は 9 クラスではなく L1–L7 の 7 クラス**。ここは台帳の書き方より狭い。

---

## 2. 消え方の 2 種類 — 監視が要るのは「静かに消える」方だけ

| 種別 | 例 | 消えたらどうなるか | 監視の要否 |
|---|---|---|---|
| **静かに消える（危険）** | L1–L7、memory.h の LP64/LLP64 | **ビルドは通る。** 挙動だけ壊れ、台帳は CURED と言い続ける | **要る** |
| うるさく消える | windows_x86_64 / linux_x86_64 等の新規 sysdepend、implicit-decl 修正 | ビルドが落ちる／リンクできない | 既存のビルドジョブで足りる |

2026-07-02 の事故が 41 日気づかれなかったのは、L1–L7 が**前者**だったから。
**したがってこのリストの本体は「静かに消える」クラスに絞る。**

---

## 3. 持ち物リスト本体（アンカーはこの run で `grep -c` により実測）

検査方法: 固定文字列 grep（`grep -c -F`）。数は **2026-09-03 の master `b264c90a` での実測値**。

パスはすべて `kernel/mtkernel3/kernel/tkernel/` 相対。

| クラス | 由来 | ファイル | アンカー（固定文字列） | 実測数 |
|---|---|---|---|---|
| **L1** wait-timer の TS_WAIT ガード | 元 `2dbacd66` | `wait.c` | `TS_WAIT) == 0` | **2** |
| **L2** KCC_DIAG 計装 | 元 `d63ff19c` | `wait.c` | `KCC_DIAG` | **2** |
| **L3a** QueRemove 後の QueInit | 元 `f109a3c4` | `wait.c` | `QueInit(&tcb->tskque)` | **1** |
| **L3b** 同上 | 元 `f109a3c4` | `wait.h` | `QueInit(&tcb->tskque)` | **1** |
| **L3c** 同上 | 元 `f109a3c4` | `timer.c` | `QueInit(&event->queue)` | **2** |
| **L3d** 同上 | 元 `f109a3c4` | `timer.h` | `QueInit(&event->queue)` | **1** |
| **L4a** enqueue ループ検出 | 元 `f109a3c4` | `timer.c` | `cnt > 10000` | **1** |
| **L4b** timer_handler ループ検出 | 元 `f109a3c4` | `timer.c` | `timer_loop_cnt > 1000` | **1** |
| **L5a** 全 TCB の wtmeb 自己リンク | 元 `8924e8d3` | `task.c` | `QueInit(&tcb->wtmeb.queue)` | **1** |
| **L5b** make_dormant のタイマ解除 | 元 `8924e8d3` | `task.c` | `knl_timer_delete(&tcb->wtmeb)` | **1** |
| **L6** knl_del_tsk のタイマ解除 | 元 `8924e8d3` | `task_manage.c` | `knl_timer_delete(&tcb->wtmeb)` | **1** |
| **L7** tk_del_tsk の ctxtsk ガード | 元 `e4e5d9d6` | `task_manage.c` | `tcb == knl_ctxtsk \|\| tcb == knl_schedtsk` | **1** |
| **MEM-UPTR** ポインタ幅マスク | `ff163ac1`（**層A**。下の訂正を読むこと） | `memory.h` | `KNL_UPTR` | **10** |

> **訂正 2026-09-03（前の run の記述は誤りだった）**
> このクラスを「層B＝`f50c30a0` に焼き込まれ `ff163ac1` が拡張」と書いていたが、**違う**。
> `git log -S'KNL_UPTR' -- kernel/mtkernel3/kernel/tkernel/memory.h` が返すコミットは
> **`ff163ac1` ただ1つ**で、`f50c30a0` 時点の `memory.h` には `KNL_UPTR` が **0 個**。
> つまりこれは **取り込み後に積んだ層A のパッチ**であって、層B の例ではない。
> **この訂正は下の陰性コントロールの解釈に直接効く**（MEM-UPTR は `f50c30a0` では
> 「消された」のではなく「まだ存在しなかった」）。
> **結果として、この持ち物リストは現時点で層B の項目を1つも含んでいない。**

### アンカー選定でハマった点（次の run が踏まないように）

- `> 1000` は **`> 10000` にも部分一致する**。L4a/L4b は必ず
  `cnt > 10000` / `timer_loop_cnt > 1000` の形で区別すること。
- L1 の `TS_WAIT) == 0` が **2** なのは、L2 の KCC_DIAG 側（`wait.c:118`）が
  意図的に同じ条件を鏡写しにしているため（`wait.c:94` のコメントが明言）。
  **L1 だけが消えても 2→1 で検出できる**が、「1 なら赤」ではなく
  「**2 未満なら赤**」と書くこと。
- 唯一の来歴マーカー `pre-f50c30a0` は **wait.c に 1 箇所しか無い**。
  systematic な目印としては使えない。

---

## 4. 陰性コントロール（2026-09-03 実施。**これが無いうちは「効く」と言わない**の約束の履行）

チェッカ本体: `tests/vendor/check_local_patches.sh`。判定は「アンカーの出現回数 >= 最低数」。
等号でないのは、同種のガードが将来増えたときに赤くしないため。

陰性ツリー: `git worktree add --detach /home/shota/pk-vendor-neg f50c30a0`
（＝2026-07-02 のベンダ差し替え直後。L1–L7 が消えていた状態そのもの）。**ブート不要・grep のみ。**

### 3アームの実測

| アーム | ツリー | 結果 | 内訳 | exit |
|---|---|---|---|---|
| 陽性 | master `8821d2e5` | **GREEN** | pass=13 lost=0 | 0 |
| **陰性** | `f50c30a0` | **RED** | pass=0 lost=13 | 1 |
| 不在（自爆テスト） | 存在しないパス | **RED** | 13 件すべて `MISSING-FILE` | 1 |

アンカー別の実測値（master → `f50c30a0`）:

L1 `2→0` / L2 `2→0` / L3a `1→0` / L3b `1→0` / **L3c `2→1`** / L3d `1→0` /
L4a `1→0` / L4b `1→0` / L5a `1→0` / L5b `1→0` / L6 `1→0` / L7 `1→0` / MEM-UPTR `10→0`

**L3c が 0 ではなく 1 なのは重要**: `timer.c` の `QueInit(&event->queue)` は 2 箇所のうち
**1 箇所が上流ベンダのコード**で、ローカルパッチは残り1箇所。
「消えたのに 0 にはならない」実例であり、**`>= 1` で書いていたら緑のまま見逃していた**。
最低数を実測で決めることの根拠がこれ。

**不在アームを入れた理由**: ツリーのパスを間違えたときに「アンカーが1件も無い＝チェックする物が無い＝緑」
と report する種類のバグ（過去に何度も踏んでいる偽成功）を潰すため。
`MISSING-FILE` は skip ではなく **fail に数える**実装になっていることを実行で確認した。

### 4.5 独立再実行（2026-09-05、別 run による再現。§6-4-4 の借りへの返済）

**限界を先に書く**: 実装者も再実行者も「指揮者代行」という同じロールで、別人格の監査ではない。
それでも別セッション・別日の再実行は「同じ run が自分の結果をコピペしただけ」というリスクは潰す。

`sh tests/vendor/check_local_patches.sh <path>` を3アームとも独立に再実行し、**数値まで完全一致**した:

| アーム | ツリー | 結果 | 内訳 |
|---|---|---|---|
| 陽性 | `/home/shota/p-kernel`（master、この run の先行コミット込み） | GREEN | pass=13 lost=0, exit=0 |
| 陰性 | `/home/shota/pk-vendor-neg`（`f50c30a0`） | RED | pass=0 lost=13, exit=1、**L3c だけ found=1**（他12件は found=0） |
| 不在 | 存在しないパス | RED | 13件すべて `MISSING-FILE`, exit=1 |

L3c が単独で `found=1`（他が0の中で）になる再現も含め、2026-09-03 の記録と完全一致。
**独立再実行の debt はここで返済したとみなしてよい**（ただし上記の限界は残る）。

### この陰性コントロールが**証明していないこと**（潰さないこと）

1. **MEM-UPTR は陰性コントロールになっていない。** 上の訂正のとおり `KNL_UPTR` は `ff163ac1` 由来で、
   `f50c30a0` には**まだ存在しなかった**。この行の RED は「消された」の再現ではなく「未誕生」。
   **正味の陰性コントロールは L1–L7 の 12 アンカー**であり、13 ではない。
2. **これは grep のトリップワイヤであって、意味の検査ではない。**
   - 文字列を保ったままロジックだけ壊す差し替え → **緑のまま通る**
   - 空白や書式を変えただけの無害な差し替え → **偽の赤**
   証明できたのは「配線が正しい（パス・アンカー・最低数の取り違えで偽の緑にならない）」ことまで。
3. **2026-08-12 のゲートと同じ強さではない。** あちらは 710 ブートの挙動測定。
   こちらは静的 grep。証拠の格が違うことを台帳に書くときも省略しないこと。
4. **書いた者と走らせた者が同じ（この run の指揮者代行）。** 実装者≠監査者の原則から、
   **独立した再実行が1回まだ借りになっている。**

## 5. まだやっていないこと（＝次の run 以降）

1. ~~CI に配線していない。~~ **2026-09-05 訂正: 配線済み（`26178169`、非BLOCKING）。CI 上で `success`
   を確認済み（陽性側のみ。陰性コントロールは push が要るので CI 上では未実証のまま）。**
2. ~~層B の完全列挙が未完了~~ **2026-09-06 に実施・完了。§7 参照。** 新アンカー6本を追加し
   計19本に。CI 上での陽性確認と、この新アンカーについての陰性コントロールの CI 上での実証は
   まだ（ホスト上でのみ実証。上記1と同じ限界）。
3. ~~独立した再実行（上記4-4）。~~ **2026-09-05 に実施。§4.5 に追記。**
4. **（新規）§7 の新アンカー6本について、独立した再実行の借りがある。** この run（指揮者代行）
   が書いて自分で3アーム走らせた。実装者≠監査者の原則から未返済。
5. ~~§7 で「対象外」と判定した4ファイルの分類は未検証~~ **同じrun内で実際にビルドして解決
   （§7.6）。config.h・machine.h・tk/syscall.hはうるさく消えるで確定、inittask.hは
   「静かに消える」に分類変更（アンカーが要る、次項6）。**
6. **（新規）`include/sys/inittask.h`のINITTASK_STKSZに`check_local_patches.sh`アンカーが
   無い。** §7.6-bの通り「静かに消える」クラスと確定したのに未対応。候補アンカーは
   `sysdepend/linux_x86_64/sysdef.h`等の`#define INITTASK_STKSZ`行そのもの
   （ヘッダの入れ替えで消える想定）。次runの宿題。
7. **（新規）§7.6の4ファイルとも`boot/linux_x86_64`/`boot/x86`のコンパイル可否のみ確認。**
   `_X86_PC_`のUSE_SUBSYSTEM等や`_LINUX_AARCH64_`/`_WINDOWS_X86_64_`ターゲットは
   個別に試していない。ブートしてinitタスクが実際に1KBスタックで落ちることも見ていない。

---

## 6. 判断が要る点（人間へ）

- **ゲートを BLOCKING にするか非 BLOCKING で1週間慣らすか。**
  台帳の closure 条件（「別のクラスを消す swap でも赤くなる」）を満たすには最終的に BLOCKING が要る。
- ~~層B をこのリストの守備範囲に入れるか。~~ **2026-09-06 に解決: 入れた。** 完全列挙ができた
  ので先送りする理由が無くなり、L1-L7・MEM-UPTR と同じ非BLOCKINGジョブに新アンカー6本
  （MEM-UPTR-C/MEM-UPTR-MP/B-SCHED-KH/B-SCHED-TM/B-SCHED-TC/B-STR）をそのまま追加した
  （計19本）。**新規の判断が要る点は下記§7の末尾を参照。**

---

## 7. 層Bの完全列挙（2026-09-06 実施）

### 7.1 前提が変わった

前run群は「上流 `435096c9` はこのリポジトリのgit履歴に存在せず、ネットワーク経由での取得も
未確認」として層Bを保留していた。**この run で `git ls-remote https://github.com/tron-forum/mtkernel_3.git`
と `git clone --bare --filter=blob:none` の両方が通ることを実測した。** VENDOR.md に上流URLが
明記されていた（`https://github.com/tron-forum/mtkernel_3`、ベース `435096c9`）ので、それを
そのまま使った。クローン先: `/home/shota/pk-scratch/mtkernel3-upstream.git`（bare、blob:none、
使い捨てスクラッチ領域）。`git cat-file -t 435096c9...` で目的のコミットが実在することも確認済み。

### 7.2 手法 — コメント除去 + 正規化した上での機械比較

VENDOR.mdが認める通り「日本語コメント刷新」で素朴な diff は巨大になり使い物にならない。
そこで **C言語のコメント（`/* */` と `//`）を文字列リテラルを壊さない範囲で除去し、
前後空白を取ったうえで行単位比較する** スクリプトを書いた
（`/home/shota/pk-scratch/layerB_diff.py`、リポジトリには置いていない使い捨てツール）。

対象: `f50c30a0`（p-kernel側、ベンダ取り込み直後）の
`kernel/mtkernel3/{kernel/tkernel,kernel/tstdlib,include,lib,config}/` 配下の `.c`/`.h` 全194ファイル。
各ファイルを上流 `435096c9` の対応パス（`kernel/mtkernel3/` プレフィックスを外すだけで1:1対応、
VENDOR.mdの記述通り）と比較した。

### 7.3 結果 — 194ファイル中、真にロジックが違うのは13ファイル

| 分類 | 件数 | 内容 |
|---|---|---|
| IDENTICAL-LOGIC | 148 | コメント除去後は上流とバイト単位で同一。日本語コメント刷新のみ。 |
| NOT-IN-UPSTREAM | 33 | 上流に存在しない新規ファイル（後述7.4、対象外） |
| **LOGIC-DIFF** | **13** | **コメントを除いても上流と異なる。ここが層Bの本体候補** |

### 7.4 NOT-IN-UPSTREAM の33件は対象外（「うるさく消える」クラス）

すべて新規sysdepend（`linux_x86_64` / `linux_aarch64` / `aarch64_virt` / `x86_pc` 用の
`machine.h`/`profile.h`/`sysdef.h`/`cpudef.h`/`dbgspt.h`/`syslib.h`）、`compat/*` ヘッダ、
`subsystem.c/h`（新サブシステム）、`libtm/sysdepend/*/tm_com.c`。**これらが消えるとビルドが
リンクできず落ちる**（新規ファイルなのでファイルごと無くなる）ので、`check_local_patches.sh`
が扱う「静かに消える」クラスの対象外。既存のビルドジョブで十分。

### 7.5 LOGIC-DIFF 13件の内訳

| ファイル | 分類 | 対応 |
|---|---|---|
| `config/config_device.h` | BOMのみ（﻿の除去） | 対象外（実質差分なし） |
| `config/config_tm.h` | BOMのみ | 対象外 |
| `include/tk/device.h` | BOMのみ | 対象外 |
| `config/config.h` | 新アーキ用 `#ifdef _LINUX_X86_64_` 等の追加ブロック（`CNF_MAX_TSKID`等の数値上書き） | **ビルドで確定（§7.6-a）: うるさく消える。** 削って`boot/linux_x86_64`を`make`すると`#error "USE_PTMR cannot be specified."`で即停止（既定値の組み合わせがそもそも無効だった）。対象外のまま |
| `include/sys/inittask.h` | 既定値への `#ifndef` ガード追加 | **ビルドで確定（§7.6-b）: 静かに消える。分類を「対象外」から変更、アンカーが要る。** ガードを外すと`make`は`rc=0`で通るが、`INITTASK_STKSZ`が`sysdepend/linux_x86_64/sysdef.h`の256KBから`inittask.h`の1KBへ警告だけを出して黙って上書きされる |
| `include/sys/machine.h` | 新アーキ用 `#include "sysdepend/..."` 分岐追加 + `#define Csym(sym) sym` | **ビルドで確定（§7.6-c）: `#include`本体はうるさく消える。** 削ると`sysdef.h`の`SYSDEF_SYSDEP()`展開が`fatal error`で失敗。`Csym(sym) sym`はp-kernel自身のターゲットから呼ばれないデッドコード（grep確認のみ、未変更） |
| `include/tk/syscall.h` | `T_DSSY` に `startupfn`/`cleanupfn`/`resblksz` 追加 | **ビルドで確定（§7.6-d）: うるさく消える。** 3フィールドとも`arch/x86/blk_ssy.c`（`kernel/mtkernel3/`の外、grepの捜索範囲外だった）が使用しており、削ると3件ともコンパイルエラー |
| `kernel/tkernel/memory.c` | LP64ポインタ幅（`UW`→現在は`KNL_UPTR`） | **既存MEM-UPTRクラス。新アンカー `MEM-UPTR-C` 追加** |
| `kernel/tkernel/memory.h` | 同上 | 既存 `MEM-UPTR` アンカーで既にカバー済み |
| `kernel/tkernel/mempool.c` | 同上 | **新アンカー `MEM-UPTR-MP` 追加** |
| `kernel/tkernel/task_manage.c` | **SCHED_RR初期化フィールド追加（新規発見）** | **新アンカー `B-SCHED-TM` 追加** |
| `kernel/tkernel/timer.c` | **SCHED_RR tick処理追加（新規発見）** | **新アンカー `B-SCHED-TC` 追加** |
| `kernel/tstdlib/string.c` | **LP64語コピー修正（新規発見）** | **新アンカー `B-STR` 追加** |

（`kernel/knlinc/kernel.h` の `SCHED_RR` フィールド定義自体は今回のファイルリストの母集合
=194ファイルには入っていなかった — `knlinc/` を対象ディレクトリに含め忘れていたため。
`task_manage.c`/`timer.c` 側の呼び出しから遡って手動で見つけ、個別に確認した。**この母集合の
抜けは正直に書く**: 194という数字は5ディレクトリの機械列挙だが `knlinc/` はその5つに
入れ忘れており、同種の見落としが他にもある可能性はゼロではない。）

### 7.6 「うるさく消える」判定の裏取り — 実際にビルドした（grepだけの版から訂正）

このセクションは一度grepだけで書いて公開したが、そのすぐ後に同じrun内で実際にビルドして
確認したところ**2箇所で見立てが外れていた**。恥ずかしい訂正ではなく、この訂正自体が
「最初の診断はしばしば間違っている。自分の申告も検算する」という憲法の実演なので、
grep版の記述を消さずに何がどう外れたかを残す。

**手法**: `git worktree add --detach /home/shota/pk-scratch/layerb-buildtest master`で
使い捨てworktreeを作り、対象ファイルを1つずつ改変しては`git checkout --`で戻し、
`boot/linux_x86_64`（ホスト gcc、クロスツールチェイン不要）と`boot/x86`
（ベアメタル、`gcc -m32`）で`make`した。CIとは重ねていない（`gh run list`で確認済み）。

**(a) `config.h`の数値`#define`群 — grep版の「静かに消える疑い」は実測でハズレ、
元の「うるさい」判定が正しかった。** 3ブロック（`_LINUX_X86_64_`等/`_X86_PC_`/
`_AARCH64_VIRT_`）を丸ごと削って`boot/linux_x86_64`を`make`したところ:
```
../../kernel/mtkernel3/include/sys/knldef.h:39:3: error: #error "USE_PTMR cannot be specified."
```
`kernel/mtkernel3/include/sys/knldef.h:38`に`#if USE_PTMR && !CPU_HAS_PTMR`という
ハードガードがあり、上書きが消えて`USE_PTMR`が既定値`(1)`に戻ると、物理タイマの無い
ホスティング環境向けターゲットでは**即座にコンパイルが止まる**。「既定値に黙って戻るだけ」
という予想は誤りで、既定値自体が両立不能な組み合わせだった。**うるさく消えるクラスで確定**。

**(b) `inittask.h`の`#ifndef`ガード — grep版の「無害」判定もハズレ。第三の分類（静かに
壊れるが、うるさく消えるクラスより見つけにくい）だった。** ガード4行を外して
`boot/linux_x86_64`を`make`したところ、**ビルドは`rc=0`で成功**したが:
```
../../kernel/mtkernel3/include/sys/inittask.h:34: warning: "INITTASK_STKSZ" redefined
../../kernel/mtkernel3/include/sys/sysdepend/linux_x86_64/sysdef.h:67: note: this is the location of the previous definition
```
`sysdef.h`が先に`INITTASK_STKSZ=256*1024`（コメント: usermainが大きなスタックを要求）を
定義しているが、ガードが無いと`inittask.h`の`1*1024`が**後勝ちで上書きする**。
Cのマクロ再定義は値が違っても警告止まり（`-Werror`は付いていない）——
**ビルドは通り、バイナリサイズも同じ4392936バイトのまま、initタスクのスタックだけ
256KBから1KBへ黙って縮む。** これはL1-L7とまったく同じ「ビルドは通るが挙動だけ壊れる」
形をしていて、しかもwarningが他の大量のwarningに埋もれるため`grep -c -F`だけでは
気づけない（redefinitionの警告文言自体は拾えるが、それが「危険な値の巻き戻り」なのか
「無害な再定義」なのかはwarning文面だけでは区別できない）。**分類を「対象外」から
「静かに消えるクラス、要アンカー」に訂正する。**

**(c) `machine.h`の`#include`分岐 — grep版の「ロードベアリングの可能性が高い」は実測で
確定。** `_LINUX_X86_64_`ブロックを削って`make`したところ:
```
../../kernel/mtkernel3/include/sys/sysdef.h:29:24: fatal error: sysdepend/TARGET_DIR/sysdef.h: そのようなファイルやディレクトリはありません
```
（`SYSDEF_SYSDEP()`マクロ展開の失敗）。**うるさく消えるクラスで確定**。
`Csym(sym) sym`がp-kernel自身のターゲットから呼ばれないデッドコードだという
grep版の指摘はそのまま生きている（今回は触っていない）。

**(d) `tk/syscall.h`の`T_DSSY`構造体拡張 — grep版の「`cleanupfn`だけ使用中」は
半分ハズレ。使用箇所は`kernel/mtkernel3/`の外にあり、3フィールド全部が使われていた。**
`startupfn`/`cleanupfn`/`resblksz`の3フィールドを削って`boot/x86`の`kernel.elf`
（`boot/linux_x86_64`ではなく——後述）を`make`したところ:
```
../../arch/x86/blk_ssy.c:80:9: error: 'T_DSSY' {aka 'struct t_dssy'} has no member named 'startupfn'
../../arch/x86/blk_ssy.c:81:9: error: 'T_DSSY' {aka 'struct t_dssy'} has no member named 'cleanupfn'
../../arch/x86/blk_ssy.c:83:9: error: 'T_DSSY' {aka 'struct t_dssy'} has no member named 'resblksz'
```
grep版は`kernel/mtkernel3/`配下だけを検索しており、**p-kernel自身の`arch/x86/blk_ssy.c`
（ベンダツリーの外）からの利用を見落としていた**。「未使用」という判定は検索範囲の
盲点で、`startupfn`/`resblksz`も実際は`NULL`/`0`を代入されている（読まれてはいないが
書かれてはいる——コンパイルは通らなくなる点では同じ）。**うるさく消えるクラスで確定**、
ただし根拠は`subsystem.c`ではなく`blk_ssy.c`だった。
また`boot/x86`の`make`（引数無し）は最初のターゲット`kloader.bin`しかビルドしない
（`kernel.elf`は`all`にしか無い）ので、`make -C boot/x86 kernel.elf`と明示する必要が
あった——これも見落としかけた罠として記録する。

**この節全体でも確認できていないこと**: (a)(b)(c)(d)とも`boot/linux_x86_64`か
`boot/x86`の**コンパイルが通るかどうか**のみを見ており、`_X86_PC_`/`_AARCH64_VIRT_`固有の
数値（config.hの`USE_SUBSYSTEM`等）や`_LINUX_AARCH64_`/`_WINDOWS_X86_64_`ターゲットは
個別には試していない。ブートして実際にinitタスクが1KBスタックで落ちることも見ていない
（警告が出ることまでしか確認していない）。

**次runへの引き継ぎ**: (b)の`INITTASK_STKSZ`は静かに消えるクラスなので
`check_local_patches.sh`にアンカーが要る。候補: `sysdepend/linux_x86_64/sysdef.h`の
`#define INITTASK_STKSZ`行そのもの（削除やコメントアウトを検知）。(a)と(c)は
うるさく消えるクラスなので既存のビルドジョブで足りるはずだが、それ自体は
「うるさく消える＝CIが拾う」という前提の確認であり、実際にCIのどのジョブが
`boot/linux_x86_64`のビルドを回しているかは未確認。

### 7.7 新発見: SCHED_RR ラウンドロビン・タイムスライスは無防備な層Bパッチだった

`kernel/mtkernel3/kernel/knlinc/kernel.h:118` のコメント:
「p-kernel 拡張: 同一優先度内ラウンドロビン（SCHED_RR）。micro T-Kernel 2.0 ポートから移植」。

これは**台帳未記載の第8のクラス**（L1–L7でもMEM-UPTRでもない）で、TCBに
`sched_policy`/`time_slice`/`remaining_slice` を追加し、`timer.c` のtick処理から
`knl_rotate_ready_queue_run()`（この関数自体は上流にも存在する標準API）を呼ぶことで、
同一優先度内のタスクを時間で回す。`docs/audit-trail.md:1905`（2026-07-03のμT-Kernel 3.0移行の
CROWN RE-BLESS記録）が「3.0のスケジューラ/時刻系syscallの中に SCHED_RR tick も含めて
標準準拠を確認した」と書いており、**移行時に一度は検証されていた**ことも確認した。
しかし**`check_local_patches.sh`にはこれを守るアンカーが1本も無かった**——
L1–L7と同じ「ビルドは通るが挙動だけ壊れる」危険クラスに該当する
（消えても即座には気づかず、ラウンドロビンが黙って消えFIFOだけになる）。

### 7.8 検証 — 4アーム（3アーム+αで実施）

新アンカー6本（`MEM-UPTR-C` `MEM-UPTR-MP` `B-SCHED-KH` `B-SCHED-TM` `B-SCHED-TC` `B-STR`）を
`tests/vendor/check_local_patches.sh` に追加し、既存18アンカーと合わせて計19本として
以下4本の木で実行（すべてこの run 内で実測、`python3` の `subprocess.run` 経由）:

| アーム | ツリー | 結果 |
|---|---|---|
| 陽性 | `/home/shota/p-kernel`（master） | GREEN, pass=19 lost=0 |
| **陰性（新規: 上流純正）** | `/home/shota/pk-scratch/upstream-neg-root`（`435096c9`をkernel/mtkernel3/構造に展開） | RED, pass=0 lost=19（全19本が0件） |
| 陰性（既存・歴史的） | `/home/shota/pk-vendor-neg`（`f50c30a0`） | RED, pass=4 lost=15（B-SCHED-KH/TM/TC と B-STR の4本だけ生存 — f50c30a0時点で既に存在していたことと整合） |
| 不在（自爆） | 存在しないパス | RED, pass=0 lost=19（全19本 MISSING-FILE） |

「上流純正」アームは今回新設した検証木で、**過去のどのアームより厳密な陰性コントロール**
（p-kernelパッチがゼロの状態そのもの）になっている。既存18本のアンカーもこの木で
初めて一括検証され、全て期待通りRED。

### 7.9 この検証が証明していないこと（潰さないこと）

1. 既存の限界（§4「証明していないこと」）がそのまま新アンカーにも当てはまる:
   文字列を残したままロジックだけ壊す差し替えは検知できない静的grepである。
2. ~~実装者と検証者が同一（この run の指揮者代行）。独立再実行がまだ借り（§5-4）。~~
   **2026-09-06、別セッションで独立再実行済み（§7.10）。** ただし限界は§4.5と同じ:
   実装者も再実行者も同じ「指揮者代行」ロールであり、別人格の監査ではない。
3. ~~`config.h`等4ファイルの分類は推測で未検証~~ **§7.6で実際にビルドして検証済み**
   （config.h/machine.h/tk-syscall.hはコンパイルエラーで確定、inittask.hは静かに消える
   側と判明）。ただし`boot/linux_x86_64`/`boot/x86`の2ターゲットでしか試しておらず、
   **2026-09-06、`_AARCH64_VIRT_`（`boot/aarch64`）と`_WINDOWS_X86_64_`（`boot/windows/x86_64`、
   mingw-w64）でも追試した（§7.11）。** `_LINUX_AARCH64_`の個別ビルドテストと、
   `_X86_PC_`のUSE_SUBSYSTEM等の個別確認、ブートしての実害確認はまだ。
4. `knlinc/`をファイルリストの母集合に入れ忘れていた（§7.5末尾）ため、
   **194という数字・13という数字は「5ディレクトリを機械列挙した範囲での」網羅性**であり、
   p-kernelの全ツリーに対する層Bの完全な保証ではない。
5. コメント除去スクリプトは文字列リテラル内の `/*` 等は保護するが、**プリプロセッサの
   継続行やトライグラフなどのC言語の隅は考慮していない**簡易実装。194ファイル中それらしい
   異常（パース崩れ）は目視で見当たらなかったが、悉皆的な確認はしていない。

### 7.10 独立再実行（2026-09-06、別セッションによる4アーム再現。§7.9項目2の借りへの返済）

**限界を先に書く**: §4.5と同じ——実装者も再実行者も「指揮者代行」という同じロールで、
別人格の監査ではない。それでも別セッション・別時刻の再実行は「同じ runが自分の結果を
コピペしただけ」というリスクを潰す。

`tests/vendor/check_local_patches.sh`（現行20アンカー、`B-INITSTK`追加後の版）を
`python3`の`subprocess.run`経由で4アームとも独立に再実行した:

| アーム | ツリー | 結果 |
|---|---|---|
| 陽性 | `/home/shota/p-kernel`（master） | GREEN, pass=20 lost=0 |
| 陰性（上流純正） | `/home/shota/pk-scratch/upstream-neg-root`（`435096c9`） | RED, pass=0 lost=20（全20本0件） |
| 陰性（f50c30a0歴史的） | `/home/shota/pk-vendor-neg`（`f50c30a0`） | RED, pass=5 lost=15（B-SCHED-KH/TM/TC・B-STR・**B-INITSTK**の5本が生存） |
| 不在（自爆） | 存在しないパス | RED, pass=0 lost=20（全20本 `MISSING-FILE`） |

`§7.8`の記録（19アンカー、f50c30a0アームpass=4）との差は**アンカー数が19→20に増えたこと
そのもの**（`B-INITSTK`が`ee8487d1`/`9c3859b8`で§7.8執筆後に追加されたため）で、既存19本の
挙動に矛盾はない。`B-INITSTK`がf50c30a0アームで生存するのも整合的——`include/sys/inittask.h`
の`#ifndef`ガードは`f50c30a0`（μT-Kernel 2.0→3.0移行）より前から存在する古いp-kernel変更で、
層Bパッチではないため。**独立再実行の debt はここで返済したとみなしてよい**
（ただし上記の限界は残る）。

### 7.11 個別ビルド確認の追試 — `_AARCH64_VIRT_` / `_WINDOWS_X86_64_`（2026-09-06）

§7.6は`boot/linux_x86_64`と`boot/x86`（`_X86_PC_`）の2ターゲットでしかconfig.hの
数値上書きブロック削除を試していなかった。使い捨てworktreeで以下を追試:

1. **`_AARCH64_VIRT_`（`boot/aarch64`、`aarch64-linux-gnu-gcc`）**: ベースラインで
   `kernel.elf`がビルドできることをまず確認（成功）。config.hの`#ifdef _AARCH64_VIRT_`
   ブロック（`CNF_MAX_TSKID`等の上書き＋`USE_DBGSPT`/`USE_PTMR`/`USE_EXCEPTION_DBG_MSG`を
   `0`にする部分）を削って再ビルドすると、`_X86_PC_`と同じ`knldef.h:39`の
   `#error "USE_PTMR cannot be specified."`で即停止。**うるさく確定**、x86の挙動と一致。
2. **`_WINDOWS_X86_64_`（`boot/windows/x86_64`、`x86_64-w64-mingw32-gcc`）**: ベースライン
   ビルドが成功することを確認（`p-kernel.exe`、PE32+、このrunでこのツールチェインでの
   ビルドを初めて実施）。ただしconfig.hの該当ブロックは`#if defined(_LINUX_X86_64_) ||
   defined(_LINUX_AARCH64_) || defined(_WINDOWS_X86_64_)`という**単一の共有分岐**であり、
   §7.6で`_LINUX_X86_64_`側から既にこの分岐の削除が同じ`#error`で確定している。
   同一プリプロセッサ分岐の削除を別マクロ経由で再テストしても新しい情報にならないため
   省略し、代わりに**`B-INITSTK`（`inittask.h`の`#ifndef`ガード）の静かな上書きが
   mingw-w64トールチェインでも再現するか**を確認した——`INITTASK_STKSZ`の`#ifndef`を
   外すと、gccと同じく`"INITTASK_STKSZ" redefined`という**警告のみ**でビルドが通り、
   値が256KBから1KBへ黙って巻き戻る（gccとmingwの2トールチェインで再現、初のクロス
   トールチェイン確認）。

残るのは`_LINUX_AARCH64_`の個別ビルド確認（config.hの分岐は`_LINUX_X86_64_`と共有なので
新情報にはならないが、ベースラインビルド自体はまだ試していない）と、`_X86_PC_`の
`USE_SUBSYSTEM`等の個別確認、ブートしての実害確認（§5の6・7、優先度は低いまま）。
