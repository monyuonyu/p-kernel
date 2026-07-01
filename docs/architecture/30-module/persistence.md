# persistence — 忘れない方舟（the ark must remember）

> Status: **SHIPPED (SLICE 0+1+2)** — 0.4.7／2026-06-19 doc-status fix. 下の
> 「実装前」「再起動で全部忘れる」は STALE。今は身元も学んだ心も再起動を生き延びる。
> What actually shipped: durable 層が正直になった（`pfs_block.c` の `pfs_dur_write` 失敗を
> 飲み込まない、`pfs_durable.c`）+ 起動時復元（`pfs_durable_restore`→`pfs_dag_restore`,
> `lm_self.c:608-614`）で **Self 系譜（SLICE 1）** が、DMN sleep の `rw[]→durable`
> 永続（`dmn.c:181`）と `st_save`/`st_load`（`student.c` / `student_shell.c`,
> `student_boot_restore`）で **学んだ心の重み（SLICE 2）** が生き残る。Android では
> `PKERNEL_PFS_DIR`（`nativeSetDataDir`）をアプリ専用領域に向けて配線済み（build.gradle.kts の
> 0.4.7 / 0.9.0 コメント参照）。残課題は本文の SLICE 3+（eviction/アポトーシス接続など）。
>
> 最終更新: 2026-06-13 ／ 関連: [gap-ledger](../gap-ledger.md)（DUR-SWALLOW 🔴 /
> DUR-REFTAB 🟠 はこの設計の SLICE 0）, [living-mind.md](living-mind.md),
> [galaxy.md](galaxy.md), [ark-profile.md](ark-profile.md), product-soul.md。

---

## 0. 何が今、生き残り、何が蒸発するか（測定済み 2026-06-13）

永続化は一枚岩ではない。心の状態は **3つの別人格** に分かれ、各々 state も
コストも違う。混同が今までの曖昧さの正体だった。

| 人格 | 何 | 現状 | サイズ |
|---|---|---|---|
| **アイデンティティ** | Self層: プロフィール（名前/ハンドル/未来への言葉）+ hash-chain 系譜 | **永続化経路あり・起動時復元あり**（`pfs_durable_restore`→`pfs_dag_restore`, `lm_self.c:613-614`, `usermain.c:609`）。Android で消えるのは `PKERNEL_PFS_DIR` 未設定だけ | ~1.3 KB |
| **学んだ心** | `rw[]` = 固定化された重み（21,568 float） | **永続化経路あり（SLICE 2 SHIPPED）**：DMN sleep が `r3_weights_persist()`（`dmn.c:238`）で `rw[]→durable` を書き、`student_boot_restore`／`st_load` で起動時に復元する。sky→blue は再起動を生き延びる（この §0 の旧「保存経路ゼロ」は STALE） | **~84 KB** |
| **教えた事実** | `r3_fq[R3_FQ_MAX=4]` engram キュー（再固定化の working set） | メモリのみ | ~数百 B |

**重要な含意**: アイデンティティは「配線一本（env）」で生き残る。学んだ心は
「新しい保存/復元経路」が要る。事実は「rw[] が残れば重みの中にいる」ので、
保存対象は **rw[]（記憶の結晶）か engram（記憶の素）か** の設計判断になる（§3）。

---

## SLICE 0 — durable 層が嘘をつくのをやめる（DUR-SWALLOW / DUR-REFTAB）

**「覚える」を作る前に、保存層が「保存した」と嘘をつくのを止める。** これを
先にやらないと、上の全スライスが砂上の楼閣になる（失敗を握り潰す層の上に
永続化を積んでも、ディスク満杯・fsync エラーで無言で消える）。

- **DUR-SWALLOW（🔴, `pfs_block.c:~233-244`）**: `pfs_put` が `pfs_ark_put` /
  `pfs_dur_write` の rc を捨てて無条件 `PFS_OK`。**修正**: durable-active かつ
  書き込み失敗なら `pfs_put` は非OK を返し、その block を **evict 不能** に
  マーク（メモリ内の唯一のコピーを FIFO eviction が捨てない）。cert:
  書き込み失敗注入 → `pfs_put` 非OK ＆ block 残存。
- **DUR-REFTAB（🟠, `pfs_dag.c:231-250`）**: ref 表（`refs.tab`）は唯一 content-
  address も CRC も無い永続状態。同長 torn で garbage head/seq をロード。
  **修正**: CRC 付与 ＋ temp+rename のアトミック書き込み（または ARK へ載せる）。
  cert: torn refs.tab はロード時に拒否され、前回の正本が生きる。

両者は監査（PR #7）が file:line 接地済み。SLICE 0 はこの 2 行を閉じる。

---

## SLICE 1 — アイデンティティが死を越える（最も安い、最も効く勝利）

「pino」という名前と同意と未来への言葉が、再起動で消えないようにする。
経路は **既に存在する** — 足りないのは Android が保存場所を知ることだけ。

- **配線**: JNI（`pkernel_jni.c`、既に `PKERNEL_NODE_ID` 等を `setenv` 済み）が
  `setenv("PKERNEL_PFS_DIR", <app getFilesDir>/ark, 1)` する。`getFilesDir()` は
  Android のアプリ専用・バックアップ対象外で安全。dir 作成は `pfs_durable.c` が
  first-use で行う（既存）。
- **これだけで** Self層の `pfs_durable_restore`→`pfs_dag_restore` が起動時に効き、
  プロフィール＋系譜が蘇る（ホストでは `PKERNEL_PFS_DIR` 既設定で動作実測済み）。
- **SLICE 0 への依存**: durable 層が正直になっていること（嘘をつく保存の上に
  アイデンティティを置かない）。
- cert `[persist-identity]`: teach 名前 → kill → 再起動 → `/self.json` に同じ
  プロフィール＋系譜が復元（content-id 一致）。Android は実機 cert（adb で
  force-stop→再起動→`/profile.json`）。

---

## SLICE 2 — 学んだ心が死を越える（sky→blue が再起動後も blue）

固定化された重み `rw[]`（~84 KB）を durable に保存し、起動時に
**復元 or（無ければ）pretrain** する。

- **保存トリガ**: 毎 teach ではなく **DMN の固定化 tick 後**（眠って整理した
  後の安定状態を保存 = 自然 ＆ flash 摩耗に優しい）。「眠ったら記憶が定着する」
  という living-mind の比喩がそのまま I/O ポリシになる。
- **保存形式**: `r3_weights_get`（既存の dtr-accessor mirror, Path W で使用）で
  `rw[]` を取り、**バージョン＋次元＋語彙 content-id のヘッダ付き** durable
  blob として書く。p-fs（content-addressed、SLICE 0 で正直化済み）に載せる。
- **復元 + ガード（wave-47 の教訓を構造化）**: 起動時、blob があれば
  ヘッダを検証 — **R_NP 次元と vocab content-id が現ビルドと一致する時だけ**
  `r3_weights_set` で載せる。不一致なら **拒否して 1 行ログ ＋ pretrain で
  作り直す**（古いフォーマットの重みを盲目ロードして基板を壊す = まさに
  salty の隣の罠を構造的に封じる）。pretrain は復元成功時はスキップ → 起動が
  速くなる副次効果（UX 宿題の一部も解消）。
- **事実キューとの関係（§3 の判断をここで確定）**: **rw[] を正本とする**。
  engram キュー `r3_fq` は再固定化の working set であって、rw[] が残れば
  RETAINED 事実は重みの中にいる（`ask sky`→blue は rw[] から答える、実測）。
  よって SLICE 2 は **rw[] のみ**を保存対象とし、engram の永続化は SLICE 3
  に送る（pending 中＝未固定化の事実だけが kill で失われる、という正直な穴を
  残してそう明記）。
- cert `[persist-mind]`: teach sky→blue → 固定化待ち → kill → 再起動 →
  `ask sky`→**blue**（pretrain 無しで即答 = 復元経路を通った証）。
  `[persist-mind-stale]`: 次元/vocab 不一致 blob → 拒否＋pretrain、答えは
  scrambled でなく chance（盲目ロードしない証）。

---

## SLICE 3（設計のみ・後続）— 未固定化の事実と eviction 圧

- **pending engram の耐久化**: SLICE 2 は固定化済みだけを守る。teach 直後・
  固定化前に kill されると、その事実は失われる（rw[] にまだ無い）。完全な
  「忘れない」には `r3_fq` の pending エントリも durable に書く必要がある。
  だが engram は小さい（数百 B）ので p-fs put で足りる見込み。
- **eviction とアポトーシスの接続**: durable が満杯に近づいた時、何を捨てるか。
  これは interoception.md のアポトーシス（本質を残して場所を空ける）と同じ
  問題系 — salience の低い古い事実から手放す。2 つの設計を将来合流させる。

---

## 正直な論点（honest issues）

- **flash 摩耗 / 電池**: ~84 KB を毎 teach 書いたら摩耗する。だから固定化 tick
  後のみ（§SLICE 2）。さらに「内容が変わっていなければ書かない」（content-id
  比較で no-op）。
- **クラッシュ整合性**: 全 durable 書き込みは temp+rename（POSIX アトミック）。
  半端な blob は次回ロードで拒否され前回正本が生きる（SLICE 0 の規律を全体に）。
- **Android のバックアップ**: `getFilesDir` は端末ローカル。クラウド同期はしない
  （方舟は群れに複製されて生き残るのであって、Google Drive に依存しない —
  これは思想的にも正しい。端末が死んでも relay 経由で隣の星に記憶がある、が
  本来の「死なない」）。SLICE 1-2 は **単機の永続化**、群れ越しの記憶は
  既存の Path E/W（LM-7/10）が担う — 2 つは直交し、両方要る。
- **何が依然失われるか（明記）**: SLICE 2 時点では固定化前の pending 事実
  （SLICE 3 で閉じる）。語彙の檻（16 語）は永続化とは別問題（別の波）。

---

## スライス順（依存）

**SLICE 0（durable を正直に）→ SLICE 1（アイデンティティ＝env 配線）→
SLICE 2（学んだ心＝rw[] 保存/復元/ガード）**。0 は 1・2 の前提。1 は安く即効
（pino が生き残る）。2 が本命（sky→blue が生き残る）。3 は後続。

cert は全部 production シンボル（`pfs_put`/`r3_weights_get`/`pfs_durable_restore`）
を叩く（sim ではなく本番経路）。実装≠監査≠監督の作法を全スライスで効かせる。
