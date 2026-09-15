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
4. ~~§7 の新アンカー6本について、独立した再実行の借りがある。~~ **2026-09-06 に返済（§7.10）。**
5. ~~§7 で「対象外」と判定した4ファイルの分類は未検証~~ **同じrun内で実際にビルドして解決
   （§7.6）。config.h・machine.h・tk/syscall.hはうるさく消えるで確定、inittask.hは
   「静かに消える」に分類変更（アンカーが要る、次項6）。**
6. ~~（新規）`include/sys/inittask.h`のINITTASK_STKSZに`check_local_patches.sh`アンカーが
   無い。§7.6-bの通り「静かに消える」クラスと確定したのに未対応。~~ **対応済み（コミット
   `ee8487d1`/`9c3859b8`、記載漏れに2026-09-07気付いて追記）。候補として挙げていた
   「`#define INITTASK_STKSZ`行そのもの」ではなく、`include/sys/inittask.h`側の
   `#ifndef INITTASK_STKSZ`ガード（`sysdepend/*/sysdef.h`の値を上書きから守っている側）を
   `B-INITSTK`としてアンカー化した——ガードが消えれば値がどう上書きされようと検知できるため、
   個別ターゲットのsysdef.hを1つずつ追うより頑健。4アーム検証済み（§7.10: 陽性19→20アンカー
   でGREEN、`f50c30a0`歴史的アームで単独生存——`f50c30a0`より前から存在するパッチのため
   正しい挙動）。mingw-w64トールチェインでの再現も確認済み（§7.11）。
7. ~~§7.6の4ファイルとも`boot/linux_x86_64`/`boot/x86`のコンパイル可否のみ確認。
   `_X86_PC_`のUSE_SUBSYSTEM等や`_LINUX_AARCH64_`/`_WINDOWS_X86_64_`ターゲットは
   個別に試していない。~~ **2026-09-06、`_AARCH64_VIRT_`/`_WINDOWS_X86_64_`/
   `_LINUX_AARCH64_`のベースラインビルドを追加確認（§7.11）。2026-09-07、`_X86_PC_`の
   USE_SUBSYSTEM/USE_LEGACY_APIの個別確認も完了（§7.13）: どちらも落とすとビルドが
   確実に壊れる（前者はリンクエラー、後者はTCBレイアウト変化による静的アサート失敗）。**
   **ブートしてinitタスクが実際に1KBスタックで落ちることも2026-09-06に単発ブートで
   試した（§7.12）: 落ちなかった。単発1回ずつなので反証にはならない**
   （このワークロードでのinitタスクのスタック使用量が1KBに収まっているだけの可能性が高い）。

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

3. **`_LINUX_AARCH64_`（`boot/linux`、`aarch64-linux-gnu-gcc`）**: ベースラインビルドが
   成功することを確認（`p-kernel`バイナリ、このrunでこのターゲットを初めてビルド）。
   config.hの分岐は`_LINUX_X86_64_`と共有のため、こちらも削除テストは省略した
   （§7.11冒頭の理由と同じ）。

これで p-kernel の全5ターゲット（`_X86_PC_` / `_AARCH64_VIRT_` / `_LINUX_X86_64_` /
`_LINUX_AARCH64_` / `_WINDOWS_X86_64_`）のベースラインビルドを一度は確認したことになる。
残るのは`_X86_PC_`の`USE_SUBSYSTEM`等の個別確認と、ブートしての実害確認
（§5の6・7、優先度は低いまま）。

### 7.12 `B-INITSTK`のブート実害確認 — 単発ブート、興味本位（2026-09-06）

§5項目7が挙げていた「実際にブートしてinitタスクが1KBスタックで落ちるか」を、
`pkernel_audit_ss`コンテナ内（`/build/pk-h/`、本番`/src`はread-onlyなのでコピーして使用）で
`boot/x86`のbaselineツリー（`INITTASK_STKSZ`=256KB）とbrokenツリー（`#ifndef`ガードを外し
1KBに固定）をそれぞれ`make clean && make all disk`し、QEMU（`qemu-system-x86_64 -kernel
bootloader.bin -serial stdio -display none -cpu qemu64`、25秒タイムアウト）で単発ブートした。

**結果: 両方とも`p-kernel>`プロンプトに到達し、`=== KERNEL EXCEPTION ===`は出なかった。**
brokenツリーはこの単発ブートでは実害を示さなかった。

**これは「無害」の証明ではない**——単発ブートを1回ずつ試しただけであり、このプロジェクトの
基準（統計的な有意差を複数ブートで取る）を満たしていない。最も可能性が高い解釈は
「このブートフィード（`init.rc`実行→ネットワーク初期化→`infer_d`起動あたりまで）での
initタスク自身のスタック使用量が、たまたま1KBに収まっている」というもので、`B-INITSTK`
アンカーの正当性（「値が黙って巻き戻ること」自体が問題）を弱めるものではない。
より深い呼び出し（例えば`ring3`/`dproc`系のshellコマンドをinitタスクの文脈で深く再帰させる
feed）を送ってから同じ比較をやり直せば違う結果が出る可能性がある。次runか人間が
優先度を上げたければ、この`/build/pk-h/`のbaseline/brokenツリーはそのまま残してある
（コンテナ内、次回も再利用可）。

### 7.13 `_X86_PC_`の`USE_SUBSYSTEM`/`USE_LEGACY_API`個別確認（2026-09-07）

§5項目7の最後の残りだった「`_X86_PC_`のUSE_SUBSYSTEM等の個別確認」に着手。使い捨て
worktree（`/home/shota/pk-scratch/x86-configtest`、作業後`git worktree remove --force`で
削除済み）で`boot/x86`の`kernel.elf`をベースライン→各アーム→ベースライン復帰の順で
`make clean && make kernel.elf`しながら確認した。

1. **`USE_SUBSYSTEM`を`0`に反転**: リンクエラーで**うるさく確定**。
   `blk_ssy.o`/`fs_ssy.o`/`net_ssy.o`（`arch/x86/{blk,fs,net}_ssy.c`、いずれも
   `USE_SUBSYSTEM`ガード無しで`tk_def_ssy`を無条件呼び出し）が`undefined reference to
   'tk_def_ssy'`、`syscall.o`が`knl_ssy_cleanup`/`knl_svc_ientry`の未定義参照で失敗。
   `tk_def_ssy`等の実体は`kernel/tkernel/subsystem.c`内で`#if USE_SUBSYSTEM`により
   丸ごとガードされている（`subsystem.h`側は`#ifndef USE_SUBSYSTEM #define
   USE_SUBSYSTEM 0`という既定値を持つが、呼び出し側の3ファイルはその既定値に
   従わず常時呼ぶため、`_X86_PC_`でこの値を落とすと確実にビルドが壊れる）。
2. **`USE_LEGACY_API`を`0`に反転**（`CNF_MAX_PORID`は`4`のまま）: **うるさく確定、
   ただし経路が想定と違った。** `undefined reference`ではなく、`cpu_cntl.c:39`の
   `_Static_assert( offsetof(TCB, isstack) == TCB_isstack, ... )`がコンパイル時に失敗。
   原因を`kernel/knlinc/kernel.h`と`kernel/tkernel/winfo.h`で追跡: `TCB.winfo`は
   `union WINFO`型で、`USE_LEGACY_API && USE_RENDEZVOUS`のときだけ`WINFO_CAL`/
   `WINFO_ACP`（ランデブ待ち情報）を含む。`USE_LEGACY_API`を落とすとこの union が
   縮み、union の直後にある`TCB.wtmeb`/`TCB.isstack`のオフセットがずれる。
   `kernel/sysdepend/x86_pc/offset.h`はアセンブリ版ディスパッチャ（`dispatch.S`）用に
   `TCB_isstack`等を**手書きの数値定数**として持っているため、構造体側のレイアウトが
   変わると静的アサートで検出される——「静かに壊れる」ではなく確実に「うるさく壊れる」
   ことが確認できた。**つまり`USE_LEGACY_API`は単なるAPI公開/非公開の切替えではなく、
   `WINFO` unionのサイズひいてはTCBレイアウト全体に波及する設定であり、`offset.h`の
   再生成なしに値を変えることはできない。**

両アームとも、config.hを元の値（`USE_SUBSYSTEM=1`・`USE_LEGACY_API=1`）に戻した上で
`git status -sb`で worktree の差分がゼロであることを確認してから削除した。
これで§5項目7は完全に消化。**`_X86_PC_`の拡張2項目はどちらも「落とせば確実にビルドが
壊れる」ことが実測で確定し、`f50c30a0`のようなベンダ差し替えでこれらの値が黙って
デフォルトへ巻き戻った場合は`B-INITSTK`（§7.6-b/§5項目6）のような「静かに消える」クラスとは
異なり、ビルド自体が失敗するため気づかずには済まない。** 新しいcheck_local_patches.sh
アンカーは不要（アンカーは「値が変わっても検出できずビルドも通ってしまう」クラスに
要るものであり、これは既にビルドで検出される）。

### 7.14 B-INITSTKのより深いフィードでの実害再測定（H'、実行者不明・このrunの外での実行）

2026-09-06のバトンが候補H'として残した「initタスクの文脈で`ring3`/`dproc`系のshellコマンドを
深く再帰させるfeedで§7.12の単発ブート比較をやり直す」について、`pkernel_audit_ss`コンテナ内
`/build/pk-h/run-h-prime.sh`という形で既に実行された痕跡を、2026-09-08 03:30台のrunが
手順1の状況把握中に発見した。**このrunが実行したものではない。** スクリプトとログの
タイムスタンプは2026-09-07 15:32〜15:36(JST)で、深夜の無人run(00:30台)の時間帯と一致せず、
実行者は不明（mk_pinoが日中に直接手を動かした可能性が高いが未確認）。バトン・ログ・この
文書のどこにも対応する記載が無かったため、埋もれさせずここに書き残す。

**方法（スクリプトを読んで確認）**: 既存の§7.12ビルド成果物（baseline=`INITTASK_STKSZ`
256KB、broken=`#ifndef`ガード除去で1KB固定、いずれも`/build/pk-h/`配下）を
`KILLCHURN_SKIP_BUILD=1`で再ビルドせずに再利用し、`tests/x86/run_killchurn.sh`
（既存のring3/dproc 5動詞feedハーネス）を`KILLCHURN_N=40`で各アーム実行。このrunで
`md5sum`により、使われたスクリプトがリポジトリ現行版（`8c0c7ad4...`）とバイト単位で
一致することを確認済み——別バージョンのハーネスにすり替わっていない。

**結果（ログを転記）**: baseline・brokenとも`sigA=0 sigB=0 clean=40 incomplete=0
other=0`（40/40 clean）。単発ブート（§7.12、N=1）より強い観測だが、依然として実害は
検出されなかった。

**正直な評価（ハーネス自身が出力に添えた注記をそのまま転記）**: 「Stage 2で測定した
無修正ツリーの発生率6.43%/boot（18/280、Wilson 95%CI 4.10-9.93%）を前提にすると、CI
下限でも真に壊れたツリーがN=40のクリーン率を通り抜ける確率は約18.74%——今回の
40/40 cleanは`brokenツリーが無害である証拠ではない`」。つまりこの40/40も§7.12の単発
ブートと同じ理由（弱い観測）で「無害の証明」ではないが、N=1からN=40への強化はできた。

**次回への含意**: H'は「一度は深いフィードで試す」という当初の目的を満たした。更に強い
観測が欲しければN=100以上に増やすか、baseline/brokenを同時実行して統計的検定にする
必要があるが、優先度はもともと低いままなので、これ以上やるかは次run/人間の判断に委ねる。

### 7.15 H'追加測定：N=100（2026-09-08、このrunが実行）

§7.14のN=40は「escape probability約18.74%」で弱かったため、同じ既存ビルド成果物
（`KILLCHURN_SKIP_BUILD=1`で再利用、再ビルドなし）に対し`KILLCHURN_N=100`で
baseline→brokenの順に再実行した（`pkernel_audit_ss`コンテナ内、`docker exec -d`で
デタッチ起動し、`Monitor`のuntil-loopで完了を検知——セッションを終わらせずに
待った。このrunで初めて`/home/shota/pk-scratch/run-h-prime-n100.sh`を作成・
`docker cp`で投入）。

**結果（ログを転記）**: baseline・brokenとも`sigA=0 sigB=0 clean=100 incomplete=0
other=0`（100/100 clean）。

**正直な評価（ハーネス自身の注記をそのまま転記）**: 「CI下限（4.10%/boot）でも、真に
壊れたツリーがN=100のクリーン率を通り抜ける確率は約1.52%」——§7.14のN=40（約18.74%）
より一桁近く強い観測になった。それでも0%ではなく、「無害の証明」ではないことは
§7.12・§7.14と変わらない。

**総括（H'系列、§7.12→§7.14→§7.15）**: N=1(§7.12、単発)→N=40(§7.14、実行者不明の
既存痕跡)→N=100(§7.15、このrun)と3段階でサンプル数を積み増したが、`B-INITSTK`の
実害はどの深さのフィードでも一度も観測できていない。これは「今回使ったブートフィード
（`ring3`/`dproc`系のshellコマンド feed）でのinitタスクのスタック使用量が、たまたま
256KBはもちろん1KBにも収まる範囲だった」可能性が最も高いという§7.12の解釈を強めた
だけで、`B-INITSTK`アンカーの正当性（「値が黙って巻き戻ること自体が問題」）を
弱めるものではない。実害を出すには、フィードの深さではなく、initタスクのスタックを
より直接的に消費させる別の攻め方（例えば意図的な深い再帰関数をinitタスク文脈で
呼ぶテストコードを書く等）が要る可能性が高いが、これは新しいテストコードの追加という
より大きな一手であり、優先度も踏まえて次run/人間の判断に委ねる。

### 7.16 §7が認めていた母集合の穴を埋めた（2026-09-10）——新規パッチ1件を発見

§7.3の脚注（7.5末尾）が「194という数字は5ディレクトリ（`kernel/tkernel` `kernel/tstdlib`
`include` `lib` `config`）の機械列挙で、`kernel/knlinc`は入れ忘れており、同種の見落としが
他にもある可能性はゼロではない」と正直に書いていた。この run はその「ゼロではない」を
実際に検算した。

**手法**: `git ls-tree -r --name-only f50c30a0 -- kernel/mtkernel3/` で母集合を機械的に
再列挙（364ファイル、うち`.c`/`.h`は341）し、§7の194ファイルリストとの差分を取った
（147ファイル)。p-kernelが実際にビルドしない他アーキ向けsysdepend（`armv7a` `armv7m`
`rxv2` `rx231` `rx65n` `rza2m` `stm32h7` `stm32l4` `tx03_m367` `iote_*` `no_device`
——RX/STM32/Cortex-A/M等のMCU向け参照ポートで、p-kernelの5ターゲットはどれもここを
通らない)を除外すると52ファイルが残った。内訳: `kernel/knlinc/`(4)・`kernel/inittask/`(1)・
`kernel/sysdepend/`直下の共通ヘッダ(5)・`kernel/sysdepend/{x86_pc,aarch64_virt,
linux_x86_64,linux_aarch64}/`(各10)・`kernel/sysinit/`(1)・`kernel/usermain/`(1)。
`windows_x86_64`のsysdependは`f50c30a0`時点でまだ存在しない(後日追加、層Aで別途
git logに載る)ため対象外——`git ls-tree`で実測済み。

§7.2と同じコメント除去+正規化スクリプト(`layerB_diff.py`)をこの52ファイルに適用。
結果: `NOT-IN-UPSTREAM`40(すべてp-kernel専用アーキのsysdepend新規ファイル、
§7.4と同じ「うるさく消える」クラスで対象外)・`IDENTICAL-LOGIC`10・**`LOGIC-DIFF`2**。

`LOGIC-DIFF`の2件:
- `kernel/knlinc/kernel.h` — 既知(§7.7のSCHED_RRフィールド定義そのもの。母集合に
  入れ忘れていただけで、パッチの存在自体は既に`B-SCHED-KH`アンカーでカバー済み)。
- **`kernel/inittask/inittask.c` — 未発見だった新規パッチ。**

**`inittask.c`の中身**: 上流は`init_task_main()`内で、`usermain()`の復帰値に関わらず
必ず`shutdown_system(fin)`を呼ぶ(関数から戻らない設計)。p-kernelは
`start_system()`失敗時のみ`shutdown_system()`を呼び、`usermain()`が正常復帰した
場合は`shutdown_system()`を呼ばず、`SYSTEM_MESSAGE`を出したうえで`tk_ext_tsk()`
(初期タスク自身の終了のみ)で抜ける——コメントいわく「p-kernelのusermainはシェル・
ネットワーク等のタスク群を起動して戻る設計のため、初期タスクだけを静かに終了し、
システムは動き続ける」。コンパイルは通ったまま挙動だけ変わる、まさに
`check_local_patches.sh`が扱う「静かに消える」クラス。もし将来のベンダ差し替えで
これが上流のまま復元されると、`usermain()`復帰後にシステム全体が
`shutdown_system()`されてしまう(電源オフ/再起動)——p-kernelの設計前提そのものを
壊す規模の退行になり得る。`f50c30a0`自身に既に入っている(層B、SCHED_RRと同種)。

**アンカー追加・3アーム検証**: `B-INITTASK-EXIT`(`kernel/inittask/inittask.c`、
文字列`tk_ext_tsk();`、min=1)を`check_local_patches.sh`に追加(20→21アンカー)。
検証結果:

| ツリー | 判定 | 全体結果 |
|---|---|---|
| `master` | OK found=1 | GREEN (pass=21) |
| `f50c30a0`(層Bの既存陰性コントロール) | OK found=1 | RED(他の理由、既知) — このパッチはf50c30a0自身に入っているので生存が正しい |
| pristine-upstream(`435096c9`、`/home/shota/pk-scratch/upstream-neg-root`) | **LOST found=0** | RED |
| 不在パス自爆 | MISSING-FILE | RED |

pristine-upstreamでのみLOSTになり、他は期待通り——新しいゲートが陰性コントロールで
赤くなることを示してから「効く」と言う、という規律を満たしている。

**この節が閉じていないこと**: 52ファイルの母集合自体も、5ディレクトリ→9ディレクトリ相当
への拡張であり、`kernel/mtkernel3/`配下を完全に汲み尽くした保証ではない
(`git ls-tree`ベースなので個別ファイルの見落としは無いはずだが、「p-kernelが実際に
ビルドする対象」の判定は人手のディレクトリ名フィルタであり、機械的な検算はしていない)。
次runがさらに疑うなら、Makefile/ビルドログから実際にコンパイルされるオブジェクトの
集合を機械的に抽出し、ソースの母集合と付き合わせる方法がより厳密。

### 7.17 §7.16の限界を機械的に検算した（2026-09-10、このrunが実行）

§7.16自身が認めていた限界——「p-kernelが実際にビルドする対象」への52ファイルへの
絞り込みは人手のディレクトリ名判断であり、Makefile/ビルドログとの機械的な突き合わせは
していない——を実際に検算した。

**手法**: p-kernelがビルドする5ターゲット全部のMakefile(`boot/x86/Makefile`・
`boot/aarch64/Makefile`・`boot/linux_x86_64/Makefile`・`boot/linux/Makefile`
[linux_aarch64]・`boot/windows/x86_64/Makefile`)を`Read`で全文読み、
`kernel/mtkernel3/`配下を参照するビルドルールを機械的に収集した。加えて
`android/app/src/main/cpp/CMakeLists.txt`という**6つ目のビルド定義**(Android
libpkernel.so、linux_aarch64ターゲットのNDK版)を発見し、同様に読んだ——これは
5ターゲットのどのMakefileにも記載が無く、`gap-ledger.md`/`vendor-patch-inventory.md`
のどこにも言及が無かった。全6ビルド定義とも`MTK3_KNL_SRCS`等の変数がすべて
**明示的な列挙(ワイルドカード無し)**なので、`make`を実際に実行しなくても
文字列一致で機械的に照合できる(実行するより厳密——実行結果のパースに人手の解釈が
入る余地がない)。

**結果**: `layerB-missing-relevant.txt`の52ファイルと6ビルド定義の参照パスを突き合わせ。

- `kernel/sysdepend/{x86_pc,aarch64_virt,linux_x86_64,linux_aarch64}/`配下の
  `.c`5ファイル×4ターゲット=20件は、全て対応するMakefileの`MTK3_SYSDEP_SRCS`に
  文字列一致——実際にビルドされることを確認。
- `windows_x86_64`が52ファイルから除外されている件は、除外理由(`f50c30a0`時点で
  ディレクトリ自体が存在しない)と、現行masterでは`boot/windows/x86_64/Makefile`が
  実際に`kernel/sysdepend/windows_x86_64/`をビルドしている事実は**矛盾しない**
  ——「今ビルドされるか」と「`f50c30a0`に焼き込まれていたか」は別軸で、後者だけが
  層Bの対象。
- ヘッダ(`kernel/knlinc/`4件、`kernel/sysdepend/`直下共通5件、
  `kernel/sysdepend/*/`配下各ターゲット5件×4=20件)は全て6ビルド定義の`INCDIRS`/
  `include_directories`に該当ディレクトリが載っており、到達可能。
- **`kernel/usermain/usermain.c`——52ファイルに含まれるが、6ビルド定義のどれからも
  参照されていない。** 実際にビルドされる`usermain.c`は全部`arch/`層の別ファイル
  (`arch/x86/usermain.c`・`arch/aarch64/usermain.c`・`arch/linux/x86_64/usermain.c`・
  `arch/linux/aarch64/usermain.c`——同名だが別ファイル、`kernel/mtkernel3/kernel/
  usermain/usermain.c`はどのビルドからも呼ばれない)。ただし`layerB-missing-results.txt`
  でこのファイルの判定は`IDENTICAL-LOGIC`(上流と同一)——ビルドされてもされなくても
  失うパッチが無いので、パッチロス検出への実害はゼロ。52ファイルの母集合の**過大包含**
  が1件見つかった、ということ。
- 逆方向(52ファイルに漏れていた、実際にビルドされる`kernel/mtkernel3/`ファイル)は
  **見つからなかった**。
- **除外側(341件から194+52=246件を引いた残り95件)も機械確認した。** `python3`で
  集合演算し、95件の内訳は`kernel/sysdepend/cpu/core/{armv7a,armv7m,rxv2,...}`
  配下63件+`kernel/sysdepend/iote_{m367,rx231,rza2m,stm32l4}`配下(各8件)32件
  ——ちょうど95件。この95件のパスパターン(`sysdepend/cpu`・`iote_`)を6ビルド定義
  全文に対して`grep`したところ**0件ヒット**——除外判断(「p-kernelが実際にビルドしない
  他アーキ向けsysdepend」)も裏付けられた。これで341ファイルの母集合は
  194(既存)+52(新規52、うち1件過大包含)+95(除外、機械確認済み)=341と、**全件の
  行き先が機械的に説明できる状態**になった。
- **194ファイル自体(§7.3の元の5ディレクトリスイープ)も独立に再検算した。**
  `full-tree-f50c30a0.txt`から`kernel/tkernel` `kernel/tstdlib` `include` `lib`
  `config`配下の`.c`/`.h`のみを`python3`で機械抽出したところ、既存の
  `layerB-filelist.txt`(194件)と**差分0件で完全一致**——独立に組んだ集合演算が
  同じ数字に着地したので、§7.3の194という数字自体の信頼度も上がった。

**結論**: §7.16の52ファイル絞り込みは、6つ全てのビルド定義と照合した結果、実質的に
正確だった。誤りは`usermain.c`1件の過大包含のみで、これは`IDENTICAL-LOGIC`判定
ゆえ実害なし。新規の未発見パッチはこの検算では出なかった——**正直な結果として書く**:
前run(§7.16)が見つけた`B-INITTASK-EXIT`が、この母集合拡張が生む最後の収穫だった
可能性が高い。副産物として、Android向けビルド定義(`android/app/src/main/cpp/
CMakeLists.txt`)がこれまでどの文書にも記載されていなかったことが分かった——
`kernel/mtkernel3`のソースリストは`boot/linux/Makefile`と完全に同一パターンで
lock-step(コメントに`tools/android/check_parity.sh`という既存の突き合わせ
スクリプトへの言及あり、層Bの範囲では新たなリスクにはならない)。

### 7.18 B-INITSTKの実害を初めて実際に再現した（2026-09-16、matched-arm N=10 vs N=10、決定的分離）

§7.15が残した宿題（「実害を出すには、initタスクのスタックをより直接的に消費させる
別の攻め方…意図的な深い再帰関数をinitタスク文脈で呼ぶテストコードを書く等が要る」）
に、このrunが着手した。結論を先に書く：**実害は実在し、決定的に再現できた
（baseline 10/10クリーン vs broken 10/10クラッシュ、同一EIP）。** ただし、その過程で
§7.12・§7.14・§7.15自身の記録に2つの訂正が必要と分かった。

**訂正1: 「baseline=256KB」は誤り。実際は8KB。** §7.12/§7.14/§7.15が使っていた
`/build/pk-h/baseline`・`/build/pk-h/broken`（`boot/x86`＝`_X86_PC_`ターゲット）を
このrunで直接確認した（`grep INITTASK_STKSZ`）。baselineの実際の値は
`kernel/mtkernel3/include/sys/sysdepend/x86_pc/sysdef.h:37`の`8*1024`（8KB）——
`inittask.h`の`#ifndef`ガードが健在で、ターゲット側の上書きが効いている。
brokenは`kernel/mtkernel3/include/sys/inittask.h`（正しいパスは
`kernel/mtkernel3/include/sys/inittask.h`、前run記載の`include/sys/inittask.h`は
リポジトリルート相対では存在しない）の`#ifndef`ガードを外し`1*1024`固定にしたもの。
256KBは`linux_x86_64`/`windows_x86_64`/`linux_aarch64`（Linux/Windowsユーザモード
ポート）側の上書き値であり、`boot/x86`が使う`x86_pc`側の値ではない——3つの
sub-entry全部がこの2つの値を混同していた。定性的な「小さい方が壊れる」という
比較の骨子自体は変わらないが、数字は8KB vs 1KBが正しい。

**訂正2（これが本題）: H'系列（§7.14/§7.15）が実害ゼロだったのは「弱い観測」ではなく
「観測対象を外していた」ため。** `ring3`/`dproc`系のshellコマンドは`arch/x86/shell.c`
のコメント通り**別タスク**（`shell_task`、`SHELL_STACK=8192`、`arch/x86/usermain.c:50,195`
の`create_task(shell_task, ...)`）の呼び出しスタック上で走る。initタスク自身は
`init_task_main()`（`kernel/mtkernel3/kernel/inittask/inittask.c:150`）が`usermain()`を
呼んだ直後、`usermain()`の`return 0`（`arch/x86/usermain.c:322`）を受けて即
`tk_ext_tsk()`（`inittask.c:168`）でタスクごと終了する——**シェルやring3/dprocの
feedがどれだけ深くても、それらはinitタスク自身の`INITTASK_STKSZ`スタックには
構造的に一度も触れない。** H'がN=1→N=40→N=100と回数を積んでも0/240だったのは、
壊れたツリーが無害だったからではなく、そもそも計測対象のスタックを一度も使う
経路を通っていなかったから。

**実験（このrun、新規）**: `git worktree add --detach`で使い捨てworktreeを2本
（`/home/shota/pk-scratch/initstk-t16-baseline`・`-broken`、いずれも`master`
`d50f600a`から分岐）作り、`arch/x86/selftest.c`に`kernel_selftest()`から呼ばれる
新関数`run_t16_stack_probe()`を追加した。これは`usermain()`の中で`kernel_selftest()`
が呼ばれる時点（`usermain.c:110`、タスク生成より前）で実行される——**initタスク
自身のスタック上で確実に動く**、既存のT1-T15と同じ実行文脈。中身は
`volatile UB pad[128]`を積む`noinline`再帰関数`pk_t16_recurse(depth)`を、深さの
リスト`{1,2,3,4,5,6,8,10,15,20,25,30,35,40}`に対して順に呼び、戻るたびに
`[SELFTEST] T16: depth=N survived r=...`を出す。broken側だけ`inittask.h`の
`#ifndef`ガードを外して`INITTASK_STKSZ`を1KB固定にした。差分は各treeとも
`selftest.c`1ファイル(+broken側は`inittask.h`2行削除)のみ（`git diff --stat`で確認）。
2本とも`docker cp`で`pkernel_audit_ss`コンテナに入れ、`tests/x86/run_killchurn.sh`
（既存のビルド・ブート・シリアルログ判定ハーネスをそのまま再利用、feedの5動詞は
今回無関係）でビルド・ブートした。

**1回目（深さ上限64、累積スタック需要ざっと128×65≒8.3KB超）**: baseline・brokenとも
`depth=64`まで全部"survived"を印字し、probe自体はクラッシュしなかった。しかし
**両方とも**probe後のサブシステム初期化中（`[kdds] K-DDS ready`の直後）に
ページフォルトで停止した——baselineは`EIP=0x00157086`(`knl_searchFreeArea`内)、
brokenは`EIP=0x00157104`(`knl_removeFreeQue`内、いずれも`kernel/mtkernel3/kernel/
tkernel/memory.c`)。無変更の対照ブート（T16なしの素の`/build/pk-h/baseline`、
同じ8KB設定）は同条件で1回ブートしてクリーン（5ゲート完走）だった。**つまり深さ64は
baseline(8KB)自身の予算も超えていた**——このtrialはbaseline/brokenの差を示す
比較にならないので、値をそのまま残しつつ較正し直した（結果を消さない、というこの
台帳のルールに従い上の段落もそのまま残した）。

**2回目（深さ上限40、累積スタック需要ざっと128×41+call/local overhead≒6.9KB —
8KB未満・1KB超と見積もって較正）**: `KILLCHURN_N=10`で両アームを再ブート
（`KILLCHURN_SKIP_BUILD=1`で再ビルドなし、同一バイナリを10回起動）。
**baseline: 10/10クリーン（全五ゲート完走、対照ブートと一致）。broken: 10/10が
同一の`EIP=0x00157104`（`knl_removeFreeQue`）でページフォルト、1回の例外もなく
完全に決定的。** matched-armで10/10 vs 0/10——Fisherの完全分離（この対比だけで
p≈2.75e-6相当、二項分布で計算可能）。probe中の"survived"印字自体はbroken側も
`depth=40`まで全部出力されており(`docker exec`ログで確認済み)、**オーバーフロー
そのものは即座には何も起こさない**——この基板にはスタックのガードページが無く
（ページング初期化`paging_init()`はkernel_selftest()より後、`usermain.c:113`）、
壊れた値がその場で使われるまで症状が出ない。

**機構についての推論（確定ではない）**: `config.h:95`で`USE_IMALLOC=1`——
`inittask.h`の`#if USE_IMALLOC`分岐により`INITTASK_STACK`は`NULL`で、実際のスタック
領域は静的配列ではなく`task_manage.c:82`の`stack = knl_Imalloc((UW)sstksz);`で
**ヒープから動的に確保される**。`knl_Imalloc`の裏側の空き領域管理
(`knl_searchFreeArea`/`knl_appendFreeArea`/`knl_removeFreeQue`、いずれも
`memory.c`)は、まさにEIPが2回とも落ちた場所——init タスクのスタックがヒープ
割り当てである以上、それを溢れさせればアロケータ自身の管理構造（隣接する
空き領域キューのノード）を直接踏みうる、という説明は自然だが、**具体的にどの
ポインタがどう書き換えられたかを命令レベルで追ってはいない**（`knl_removeFreeQue`の
`cmp DWORD PTR [eax],0x0`がフォルトしていることは`objdump`で確認済み——引数の
ポインタ自体が無効値になっている）。深追いすればアドレス単位の証明ができるはずだが、
優先度とのバランスから次run/人間の判断に委ねる。

**正直な限界**: (1) このT16は合成的なprobeであり、現在出荷されているブート経路の
どこも、initタスク自身のコンテキストでこの深さの再帰を実際には行わない——
「危険が実在し到達可能」の証明であって、「今のブートが危険」の証明ではない。
(2) 実装者=検証者（このrun自身が設計・実行・解釈の全てを行った）——ただし
10/10 vs 0/10・同一EIPという決定性は、測定ノイズで説明する余地をほぼ残さない。
(3) 較正ミス（1回目の深さ64）は両アームを壊すという形で見つかったが、これ自体も
「ガードページが無く症状が遅れて出る」という発見を補強する副産物として残した。

**この発見の意味**: `B-INITSTK`アンカー（§5項目6、`check_local_patches.sh`の
既存アンカー）が守っている対象は、これまで「値が黙って巻き戻ることそれ自体が
問題」という理論的な正当化しかなかった。今回、初めて実害の実物（決定的な
ページフォルト、10/10）を作れた。**ゲートの位置づけ（非BLOCKING、CI配線済み、
陰性コントロール済み）は変わらない**——この発見は「効くはずの理由」を実証した
だけで、BLOCKING化の判断（§6・判断待ち項目1と同型）を変えるものではない。
使い捨て資産は`pkernel_audit_ss`コンテナの`/build/initstk-t16-baseline/`・
`/build/initstk-t16-broken/`と、ホスト側`/home/shota/pk-scratch/initstk-t16-baseline/`・
`-broken/`（`git worktree`、detached HEAD `d50f600a`）に保持——リポジトリの
トラッキング対象ファイルは一切変更していない（`git -C /home/shota/p-kernel status`
はクリーン）。

### 7.19 B-INITTASK-EXITの実害も実際のブートで再現した（2026-09-16、matched-arm N=3 vs N=3、決定的分離）

§7.16が見つけた層Bパッチ`B-INITTASK-EXIT`（`kernel/mtkernel3/kernel/inittask/
inittask.c`、`usermain()`が正常復帰したとき上流は`shutdown_system(fin)`を
無条件で呼ぶが、p-kernelは`tk_ext_tsk()`で初期タスクだけを終えてシステムは
動き続ける）も、§7.16/§7.17時点では静的diffとアンカーのビルド確認のみで、
「実際にこの差し替えが起きたらブートはどう壊れるか」は未実証だった。§7.18の
B-INITSTKと同じ日にこれも解消した。

**実験**: 使い捨てworktree2本（`/home/shota/pk-scratch/inittask-exit-baseline`・
`-reverted`、いずれも`master` `6248bbdd`——§7.18コミット直後——から分岐）。
baselineは無変更。revertedは`inittask.c:162-168`の該当ブロックを、上流相当の
`shutdown_system(fin);`の無条件呼び出しに変更（p-kernel側コメント込みで
11行→4行に削減、差分はこの1ファイルのみ、`git diff --stat`で確認）。
`docker cp`で`pkernel_audit_ss`に投入、`tests/x86/run_killchurn.sh`
（`KILLCHURN_N=3`）でビルド・ブート。

**結果**: baseline 3/3クリーン（全五ゲート完走）。reverted **3/3が`INCOMPLETE`
（feed stalled at stage 0/6）**——3回とも同一のシリアルログ末尾:
`[net] Sending ARP request...` → `[OK] Net RX task`（shell/net等のタスク生成が
usermain()内で完了した直後）→ `<< SYSTEM SHUTDOWN >>` → 90秒のハーネスタイムアウトで
QEMU強制終了。`shutdown_system()`（`inittask.c:95`）は`knl_finish_device()`後に
このメッセージを出し、`knl_tkernel_exit()`（`sysinit.c:131`）が`knl_timer_shutdown()`
でシステムタイマを止めてから`while(1);`で永久停止する——タスクは生成されていても
一度も走らない（初期タスクがusermain()から戻った時点でまだ他のタスクへの
ディスパッチが起きていないため）。

**この検証にB-INITSTKほどの反復が要らない理由**: フィード再現性の議論（バースト性の
KCC-WILDPC/STALE-WAIT-TIMER）とは違い、`fin`は`usermain()`の`return 0;`
（`arch/x86/usermain.c:322`）由来の**コンパイル時に決まった固定値**で、割込み
タイミング等の競合に一切依存しない。3/3 vs 3/3の分離は、タイミング次第で
ときどき起きる現象ではなく、コードパスの分岐そのものの結果——N=3で十分、
増やしても情報量は増えない。

**この発見の意味**: `B-INITTASK-EXIT`アンカーが守る対象は、「電源オフ/再起動が
スプリアスに起きる」という抽象的な記述だったが、実際の被害はそれよりも
はるかに大きい——**シェルもネットワークも一切機能する前に全システムが完全停止する**
（p-kernelはmicro T-Kernel 2.0時代の意味論、usermain()がシェル等を起動して
戻る設計に依存しているため、上流のmicro T-Kernel 3.0の意味論に戻すと
実質的に「起動しない」に等しい）。B-INITSTKの「静かに壊れて、症状が遅れて別の場所に
出る」というパターンとは対照的に、これは「静かに壊れる（ビルドは通る）」が
症状は即座かつ全面的——ただし自動テストが単に「ビルドが通るか」だけを見ていれば
気づかれない、という§2の分類基準そのものには当てはまる。ゲートの位置づけ
（非BLOCKING、CI配線済み）は変わらない。使い捨て資産は`pkernel_audit_ss`の
`/build/inittask-exit-baseline/`・`-reverted/`とホスト側worktree（実験後に
`git worktree remove --force`で削除済み）。リポジトリのトラッキング対象ファイルは
変更していない。
