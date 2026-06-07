# p-fs — 内容アドレスで gossip 複製される、死なないファイルシステム

> p-kernel は「全体で1つの脳」である。ならば**記憶もまた、1台の中に閉じてはならない。**
> 今のファイルシステムは IDE 上の FAT32 — 1台のディスクに縛られた、中央を持つ記憶である。
> このドキュメントは、その placeholder を、**内容アドレス・gossip 複製・履歴保存・
> 消失訂正符号**を備えた分散記憶 `p-fs` へ再設計するための一枚の絵である。
> 目指す終着点はひとつ — **「ファイルを保存する」と「クラスタに公開する」の境界を消すこと。**

Status: **設計のみ（未着手）** / 最終更新: 2026-06-06

関連: [[regions.md]]（遅延クラスタ region — p-fs の複製局所性はこれを反復する）、
[[survival-network.md]]（§9「保存の図書館 → 考える器官」。記録の継続は思考の前提条件）、
[[project_pkernel_philosophy]]（5レイヤー世界観の Body/Collective 層）、
`replica.c`（トピックの gossip 複製＝p-fs の先行実装）、`sfs.c`（`/shared` 同期＝最も近い既存資産）。

---

## 0. なぜ今これを書くか

`survival-network.md §9` は p-kernel の位置づけをこう更新した：

> 旧: 人類の記録を未来へ残す「保存の図書館」
> 新: 人類の知の総体が、必要なときに必要な場所で思考する「考える器官」
> 記録の保存は、思考の前提条件にすぎない（記憶がなければ考えられないから）。

つまり**記憶の保存は思考の土台**である。にもかかわらず、現在のファイルシステムは
その土台にふさわしくない。Collective 層（gossip pub/sub・SWIM・region・replica）は
「ノードが死んでもデータは隣に残る」サバイバビリティを既に持っているのに、
**ファイルだけがそこから取り残されている。**

### 0.1 現状の正確な姿

| 層 | 実体 | 性質 |
|---|---|---|
| 永続化バックエンド | `arch/x86/fat32.c` + `boot/x86/kl_fat32.c` | **1台の IDE ディスク上の FAT32** |
| 抽象化 | `arch/common/include/vfs.h` | 「FAT32 への薄いラッパ」と自称（コメント L4-7） |
| クラスタ同期 | `arch/common/sfs.c` | `/shared` 配下だけを全 ALIVE ノードへ複製（独自 UDP 7381） |
| 非 x86 | `arch/*/vfs_stub.c`（aarch64 / linux） | **スタブ。ファイルシステムが無い** |

`vfs.h` 自身のコメントがこの貧弱さを白状している：

> Currently: one mount point (FAT32 on IDE drive).
> Future: multiple backends (ramfs, devfs, …).

### 0.2 FAT32-as-placeholder の何が「ノード死耐性のある脳」にとって致命的か

1. **中央を持つ記憶**。FAT32 は1台のディスク上の単一の木構造。その1台が死ねば、
   そのファイルは消える。`survival-network.md §2`「中央を持たないことそのものが
   生存力の源泉」という不変条件に、**ファイルシステムだけが違反している。**
2. **可変・上書き**。FAT32 はブロックを上書きする。`survival-network.md §9`
   「記憶がなければ考えられない」の記憶が、**履歴を残さず書き潰される。**
3. **path = 場所**。`/foo/bar.txt` は「このディスクのこの位置」を意味する。
   クラスタ全体で同じファイルを指す**位置非依存の名前**が無い。
4. **同期が後付けの特例**。`sfs.c` は `/shared` という特別なフォルダだけを、
   サイズ上限 32KB・同時受信 1 ファイル・全対全ブロードキャストで複製する。
   これは「保存」と「公開」を**別々の操作**として持っている。本来は同じであるべき。

### 0.3 ローカルファイルとクラスタオブジェクトの断絶

今、ノードがファイルを書くと FAT32 に落ちる（ローカル）。それを共有したければ
`/shared` に置いて `sfs_push()` で全員に押し付ける（クラスタ）。この**2段構え**が
問題の本質だ。

`kdds.h` のコメントは「すべてはトピック」を掲げ、Unix の「すべてはファイル」に
対置した。だが**ファイルだけがトピックになっていない。** p-fs のゴールは、
この最後の島を Collective 層に橋渡しすること — **save == publish** である。

---

## 1. テーゼ：4つの設計原則は「save == publish」の4つの面

| 原則 | 何を捨て、何を得るか |
|---|---|
| ①内容アドレス | path（場所）を捨て、**hash（中身）で名指す**。同じ中身は宇宙で1つ。 |
| ②gossip 複製 | 「中央サーバへ保存」を捨て、**replica/SWIM/region で勝手に増える**。 |
| ③履歴保存 | 上書きを捨て、**append-only の Merkle-DAG** で過去を消さない。 |
| ④消失訂正符号 | 全複製（N倍コスト）を捨て、**少ない冗長で N 台喪失に耐える**。 |

この4つは独立した機能ではない。「中身で名前が決まる（①）から、どこに置いても
同じ（②できる）し、版が連なっても中身が変わらない過去は不変（③）であり、
中身をシャードに割って散らせる（④）」——**「ファイルを公開する」という一つの
行為の、4つの側面**である。`regions.md §2` の「3つの面は1つのアーキテクチャ」と
同じ構造をしている。

---

## 2. 設計原則（詳細）

### 2.1 内容アドレス — hash で名指す不変ブロック

ファイルは可変の「場所」ではなく、不変の「中身」の集合になる。

```
block      = 任意の <= BLOCK_MAX バイトのバイト列
block-id   = H(block)          # H = sha256（relay/sha256.c に既存・ゼロ依存）
object     = block-id の有向リスト（大きいファイルはチャンク分割）
object-id  = H(object のマニフェスト)
```

- **block-id は中身から決定的に決まる。** 同じ中身は、どのノードが書いても同じ id。
  → 重複排除が自動で効く（同じセンサログ・同じモデル重みは宇宙に1コピー）。
- **不変（immutable）。** 一度書いた block は二度と変わらない。変更＝**新しい block**。
  これが③履歴保存と④符号化を素朴にする（不変なものは安全にコピー・分割できる）。
- `relay/sha256.c` は Phase B relay v2 のために**スクラッチで書いたゼロ依存実装**
  （メモリ `moment_2026_05_26_phase_b_v2`）。これをカーネル側へ持ち込めば、新規の
  暗号依存ゼロで内容アドレスが立ち上がる。

> なぜ hash 名か：位置非依存の名前は「中央を持たない」ための前提条件である。
> path は「どのディスクの何処」を含意するが、hash は中身だけを含意する。
> どのノードに在ろうと、in-region だろうと cross-region だろうと、同じ block は
> 同じ名前で呼べる。**[[survival-network.md]] §2 の不変条件をファイルにも適用する。**

### 2.2 gossip 複製 — replica/SWIM/region をそのまま使う

新しい複製エンジンは作らない。**既にある `replica.c` の思想を block に適用する。**

`replica.h` のコメントが既にゴールを語っている：

> これにより「最後の 1 ノードが生き残れば全記憶が保存される」というサバイバビリティが
> 実現される。

p-fs はこれを「トピックの値」から「block」へ拡張する。ただし `regions.md` の
**局所性原則を反復する**：

| 範囲 | 複製密度 | 担い手 | 既存の写像 |
|---|---|---|---|
| **region 内（密）** | 高い複製係数 r_in（例 3） | region メンバが互いに保持 | `KDDS_SCOPE_REGION` 配送（`kdds.c:199`） |
| **region 間（疎）** | 低い複製係数 r_out（例 1〜2） | coordinator 間で要約/シャードのみ | `KDDS_SCOPE_GLOBAL` 配送（coordinator ゲートウェイ） |

- region 内では**密に複製**して即時の読み出し（反射層／`survival-network.md §8`）を速くする。
- region 間は**疎に**して O(N²) を殺す（`regions.md §3.4` のスコープ階層をそのまま使う）。
- block の置き場所は **block-id を鍵とした一貫性ハッシュ**で決める（§3.3）。これにより
  「誰がこの block を持つべきか」を中央索引なしに全員が同じ計算で導ける。

### 2.3 履歴保存 — append-only / Merkle-DAG

ファイルは「最新版」ではなく「版の連なり」になる。

```
version = { object-id, parent-version-id, author-node, logical-clock, meta }
ref     = 人間可読な名前 → 最新 version-id へのポインタ（可変なのはここだけ）
```

- version は parent を1つ（または複数：マージ）指す。**過去の version は不変**で
  あり、その object と block は消えない（GC するまで。§6.3）。
- これは git の object model と同型：blob（block）/ tree（object マニフェスト）/
  commit（version）。違いは**保管場所が1台のディスクではなく gossip メッシュ**である点。
- `ref` だけが可変で、`replica.c` の seq 比較マージ（より新しい seq を採用）に
  そのまま乗る。**衝突は version-DAG の分岐として保存し、後でマージ**できる
  （上書きで過去を失わない）。
- `mem_store.h` の「人類の営みの記録を永遠に残す」基盤は、この append-only DAG の
  最初の本物のユーザになる（現状は FAT32 `/mem/YYYYMMDD.mem` + SFS 複製）。

### 2.4 消失訂正符号（erasure coding）— N 台喪失に全複製コスト無しで耐える

全複製は安全だが高い：r=3 なら 3 倍のストレージ。大きい object（モデル重み・
長い記録）には Reed–Solomon 系の **(k, m) 符号**を使う。

```
object を k 個のデータシャードに分割
+ m 個のパリティシャードを計算 → 計 (k+m) シャード
任意の k シャードが揃えば object を完全復元できる（= 最大 m 台の喪失に耐える）
ストレージ膨張は (k+m)/k 倍（例 k=6,m=3 → 1.5 倍で 3 台喪失耐性）
```

- シャードもまた内容アドレスされた block（shard-id = H(shard)）。よって §2.2 の
  gossip 複製にそのまま乗る。
- **配置は region をまたぐ**：m 個のパリティを別 region に散らせば、**1 region 丸ごと
  喪失**（半球の消失。`regions.md` の比喩）に耐えられる。これが erasure coding を
  region 設計と噛み合わせる理由。
- 小さく・ホットな block（ref・最新 version・小ファイル）は**全複製**のまま
  （符号化のオーバーヘッドが見合わない）。**大きく・コールドな block だけ符号化**する。
  判断は block サイズとアクセス頻度で連続的に（`degrade.c` の capacity 連続関数と同じ精神）。

> 正直に：erasure coding は p-fs で**最も新規性が高く・最も難しい**部分（§6.2）。
> 全複製（§2.2）だけでも「最後の1ノードで記憶が残る」は達成できる。符号化は
> **コスト最適化**であって、生存の前提条件ではない。だから実装シーケンス（§4）では
> 最後に置く。

---

## 3. アーキテクチャ

### 3.1 4層モデル

```
  ┌─────────────────────────────────────────────┐
  │ namespace 層   ref: 人間名 → version-id        │  可変・gossip（replica seq マージ）
  ├─────────────────────────────────────────────┤
  │ version 層     Merkle-DAG（version→object→…）  │  不変・append-only
  ├─────────────────────────────────────────────┤
  │ object 層      manifest: block-id[] / shard 配置 │  不変・内容アドレス
  ├─────────────────────────────────────────────┤
  │ block 層       block-id = H(bytes) の不変塊      │  不変・内容アドレス・符号化対象
  └─────────────────────────────────────────────┘
            ↓ 全層が下記の「メッシュ基盤」に乗る
   SWIM 膜（membership）/ region（局所性）/ K-DDS（scoped 配送）/ replica（gossip 複製）
```

vfs.h の現 API（`vfs_open/read/write/...`）は**そのまま温存**し、その下に
FAT32 と並ぶ第2のバックエンド `p-fs` を挿す。`vfs.h` コメントの "Future:
multiple backends" を実装する形になる。アプリは API 変更なしで p-fs に載れる。

### 3.2 書き込み経路 — save == publish

```
vfs_write(fd, buf, len)
  → チャンク分割 → 各チャンクを block 化（block-id = H(chunk)）
  → 既に in-region に在る block-id はスキップ（重複排除）
  → 新規 block を §3.3 の責任ノードへ gossip（KDDS_SCOPE_REGION で密、必要なら GLOBAL）
  → object マニフェスト（block-id 列）を block 化
  → version を作成（parent = 旧 ref の指す version、author=自ノード）
  → ref を新 version-id へ更新（replica.c の seq マージに乗せて全員へ）
```

この一連が**「保存」かつ「公開」**である。`/shared` のような特別扱いは要らない。
**書いた瞬間にクラスタの object になる。** これが §0.3 の断絶を消す。

> 局所性の既定：書き込みノードの region に密に置き（反射層が速く読める）、
> region 間へは ref と（符号化シャードのうちの一部だけ）疎に流す。
> ホットなものは近く、コールドな冗長は遠く——`survival-network.md §8` の二層と同型。

### 3.3 読み出し経路 — 中央索引なしで block を見つける

**これが p-fs の心臓部であり、最大の難問**（`regions.md §6`・survival §7 が
「中央なしのルーティング」を核心と呼んだのと同じ構造）。中央メタデータサーバは
**置かない**（不変条件）。block-id から所在を引く方法の候補：

1. **一貫性ハッシュ（DHT 風・第一候補）**：block-id を SWIM の ALIVE ノード ID 空間へ
   写像し、「id に最も近い r 台」を責任ノードとする。全ノードが同じ membership
   ビュー（`dnode_table[]`）から**同じ計算**で責任集合を導けるので、索引が要らない。
   読みは「責任ノードへ直接リクエスト」。churn 時は責任集合がずれるが、§3.4 の
   自己修復が吸収する。
2. **gossip lookup（フォールバック）**：責任ノードが応答しない/ビューがずれている
   ときは、`region 内` に `WANT(block-id)` を投げ、持っている者が返す。region 内に
   無ければ coordinator 経由で `global` に問い合わせる（疎・要約のみ）。
3. **局所キャッシュ**：一度引いた block-id → 所在ノードは EWMA 的に覚える
   （region.c の RTT EWMA と同じ手法）。ホットパスは索引なしで当たる。

> 設計判断：**①一貫性ハッシュで「べき所」を全員が決定的に計算でき、②外れたら
> gossip で実際を探し、③成功を局所に覚える。** 完全な大域索引は持たず、
> eventual に正しい所在へ収束させる。`survival-network.md §7` の「局所シグナルの
> 勾配で全体最適へ収束」を、ルーティングではなくロケーションに適用した形。

### 3.4 自己修復 — 失われたシャードを取り戻す

ノードが DEAD になると、そのノードが責任を持っていた block/shard の複製数が減る。
**既存の自己修復機構をデータにも効かせる：**

| 既存機構 | 現在の対象 | p-fs での役割 |
|---|---|---|
| `swim.c` → DEAD 遷移検出 | タスク | block 複製数の低下トリガー |
| `heal.c` `heal_on_node_dead()`（heal.c:106） | ガード済みタスクを後継ノードで再起動 | **後継ノードが、失われた block の責任を引き継ぐ** |
| `replica.c` `replica_boot_cry()`（replica.c:273） | 復帰ノードが全記憶を再取得 | 復帰ノードが担当 block を**再取得して再複製** |
| `degrade.c` capacity(N) | 分散戦略バンド | 残存台数に応じて複製係数 r を連続調整 |

- DEAD を検知したら、`heal.c` と同じ「生存ノードで最小 ID を後継に選ぶ」決定的規則で
  **失われた複製の新しい責任ノードを選び**、生き残ったコピー（or 符号化なら k シャード）
  から再生成する。
- 符号化 object は、k シャードが揃う限りパリティから**消えたシャードを再計算**できる
  （Reed–Solomon の復元）。これが全複製より省ストレージで N 台喪失に耐える所以。
- 復帰ノードの `replica_boot_cry()` は「ファイル版 Boot Cry」を `sfs.c` で既に
  実装済み（`sfs_boot_sync()`）。p-fs ではこれを block 粒度で行う。

### 3.5 一貫性モデル — eventual consistency、何を保証し何を保証しないか

- **block / object / version は不変**なので、それらについては**強い保証**：
  block-id が一致すれば中身は必ず一致する（hash の衝突耐性に帰着）。**読みは
  常に「自分が見ている version の世界」では一貫**している。
- **可変なのは ref（人間名→最新 version）だけ**。ここは `replica.c` の seq マージで
  **eventually consistent**：一時的に古い ref を見ることはあるが、gossip が収束すれば
  最新へ揃う。同時更新は version-DAG の分岐として**両方保存**し、衝突を失わない
  （last-writer-wins で過去を消さない）。
- **保証しないこと**：線形化可能性（linearizability）・全順序。`regions.md §7`
  「同期した1枚のテンソルにはしない」「非同期・eventual を受け入れる」という
  非目標を、ファイルシステムでも踏襲する。強整合が要る稀なケースのみ region 内
  `raft.c` で ref を合意（オプション、§6 の論点）。

---

## 4. 既存資産へのマッピング

「何が在り・何が新規か」を正直に分ける。p-fs は**新しい分散システムではなく、
既存メッシュ基盤の上に立つ第2のファイルバックエンド**である。

| p-fs の関心事 | 既存モジュール（流用） | 新規に要るもの |
|---|---|---|
| 内容アドレス（hash 命名） | `relay/sha256.c`（ゼロ依存・既存） | カーネル側へ移植、block 抽象（`pfs_block.c` 新規） |
| gossip 複製 | `replica.c`（seq マージ・boot cry）/ `swim.c`（membership） | block 粒度への一般化、責任集合計算 |
| 局所性（密/疎複製） | `region.c`（RTT クラスタ）/ `kdds.c` の `KDDS_SCOPE_REGION/GLOBAL` | 複製係数 r_in/r_out のポリシ |
| 配送 | `kdds.c` scoped pub（kdds.c:199-205）| block 転送は K-DDS 128B 制限超のため `sfs.c` 型の独自 UDP を一般化 |
| 履歴（append-only DAG） | `mem_store.c`（記憶の永続化思想）| version/object マニフェスト、ref ポインタ（`pfs_dag.c` 新規） |
| 自己修復 | `heal.c`（後継選出）/ `replica.c`（再取得）/ `degrade.c`（capacity(N)）| 「複製数低下」トリガーと再複製/再符号化 |
| 既存フォルダ同期 | `sfs.c`（`/shared` 同期・tombstone・chunk 転送・boot sync）| **p-fs の prototype として吸収**（§4.1） |
| VFS 抽象 | `vfs.h`/`vfs.c`（API は温存）| FAT32 と並ぶ p-fs バックエンドの挿し込み |
| 永続バックエンド | `fat32.c`（当面のローカル下層ストア）| block を物理に落とす store（最初は FAT32 上、後に専用） |
| 消失訂正符号 | — | **完全新規**（Reed–Solomon、`pfs_ec.c`） |

**`sfs.c` は最も近い既存資産**である：chunk 転送（512B）・tombstone・起動時同期
（boot cry）・「特定フォルダだけ全 ALIVE に複製」——p-fs が一般化したい挙動を
**小さく・特殊化した形で既に持っている**。p-fs は sfs を捨てるのではなく、
その「同期する範囲」を `/shared` 限定から content-addressed block 全域へ、
「全対全」から region-scoped + 一貫性ハッシュへ**広げたもの**として育てる。

**不変条件（壊さない約束、`regions.md §4` と同じ精神）**：
- 単体（SOLO）で完結する読み書きパスを常に残す。**孤立ノードもファイルを持てる脳**。
- p-fs は**論理層**。relay 無し・SWIM だけの素のメッシュでも複製が回る。
- 異種 ABI（aarch64 + x86_64 + Android）を跨いで block-id は一致する
  （hash は ABI 非依存。`feedback_lp64_typedef_trap` の轍を踏まぬよう block-id は
  固定幅バイト列で持つ — `long` 由来の幅依存型を id に使わない）。
- **中央メタデータサーバを置かない**（[[survival-network.md]] §2・§7 の核心）。

---

## 5. 段階的実装シーケンス（P0→P4）

`regions.md` の R0→R3 に倣い、**最小で役に立つものから**。各段は単体でも価値が出る。

| 段階 | 内容 | 役立ち | 依存 |
|---|---|---|---|
| **P0**（土台） | `relay/sha256.c` をカーネルへ移植。block 抽象（block-id=H(bytes)）と、FAT32 上に block を内容アドレスで落とす content-addressed store。重複排除がローカルで効く。 | 同じ中身を二度保存しない。`vfs.h` の第2バックエンド枠を作る。 | 非分散でも価値あり |
| **P1**（sfs 一般化） | `sfs.c` の chunk 転送・tombstone・boot sync を **block 粒度**へ拡張。`/shared` 限定をやめ、任意 block を **region-scoped gossip**（`KDDS_SCOPE_REGION`）で複製。 | 「最後の1ノードで記憶が残る」がファイルにも効く。save==publish の第一歩。 | swim/region/kdds（既存） |
| **P2**（履歴 DAG） | object マニフェスト + append-only version-DAG + ref ポインタ。ref を `replica.c` の seq マージに乗せる。`mem_store` を最初の本物ユーザに。 | 上書きで過去を失わない。版を遡れる。 | replica（既存） |
| **P3**（分散ルックアップ） | block-id の一貫性ハッシュで責任集合を決定的に計算。外れたら gossip lookup（WANT）。局所キャッシュ。`heal.c`/`degrade.c` で複製数を自己修復。 | 中央索引なしで読みが当たる。ノード死で複製が自動回復。 | heal/degrade（既存）+ 新規責任計算 |
| **P4**（消失訂正符号） | 大きく・コールドな block に Reed–Solomon (k,m)。パリティを **cross-region 配置**。1 region 喪失に耐える。 | 全複製の N 倍コスト無しで N 台喪失耐性。 | **新規 `pfs_ec.c`**、P1-P3 全部 |

> 鶏と卵への正直な答え（`regions.md §5` と同型）：P0–P2 は**玩具スケールでも
> 正しく作れる**（記憶のトポロジの話だから）。P3 の分散ルックアップが「区切る意味」を
> 本当に持つのは、データが1台に収まらなくなってから。P0–P2 で**配管**を通し、
> P3–P4 で**規模と冗長**を上げる。Phase D（Android フリート）が、P1–P3 を
> 机上でなく現実の churn にさらす強制力になる（[[regions.md]] §5 と同じ）。

---

## 6. 正直な論点 / 未解決

### 6.1 中央索引なしの分散ルックアップ（最難関・survival §7 と同じ壁）

一貫性ハッシュは「べき所」を決定的に計算できるが、**membership がノード間で完全一致
しない**（`region.h` コメント L11-16 が egocentric ビューの非一致を白状している）。
A が責任と思うノードと B が責任と思うノードがずれると、書いた block を読み手が
引けない瞬間が生じる。gossip lookup のフォールバックがどこまで吸収できるか、
churn 率に対するルックアップ成功率は実測しないと分からない。**`regions.md §6.2`
「ルーティング表の一貫性」と同じ問題が、ファイルでも出る。**

### 6.2 churn 下の符号化シャード配置

erasure coding は「シャードがちょうど良く散っている」前提で成り立つ。だが
ノードは絶えず join/leave する。喪失でパリティが目減りするたびに**再符号化**が
要るが、再符号化は k シャードを集めて計算する重い操作で、それ自体が帯域を食う。
「いつ再符号化するか」「region 跨ぎのパリティをどこに置き直すか」を churn の最中に
中央なしで決めるのは未解決。**安直にやると `feedback_hosted_relay_stack_overflow`
の轍（重い処理をホットパスに入れて落ちる）を踏む。**

### 6.3 履歴のガベージコレクション

append-only は美しいが、**永遠に増える**。`survival-network.md §9` は「永遠に残す」と
言うが、物理ストレージは有限。どの古い version を捨ててよいか（到達不能な version、
古すぎる分岐）を、**分散で・中央なしで・他ノードがまだ参照しているかも知れない中で**
安全に判断するのは難問。分散 GC は古典的に難しい（参照カウントは gossip で正確に
取れない）。当面は「捨てない」で始め、容量逼迫を `degrade.c` 的シグナルで検知して
から考える、が現実解か。

### 6.4 block-id の検証コストと信頼

受信した block が本当にその id（H(block)）かは、受け手が再ハッシュして検証できる
（内容アドレスの利点）。だが**ref（可変）は誰でも更新できてしまう**。誰の ref 更新を
信じるか——region をまたぐ信頼は `regions.md §6.4`「region 間の信頼（HMAC は
リンク単位）」と同じ未解決問題に帰着する。署名付き version（author の鍵で署名）が
要るかもしれないが、鍵管理は別レイヤー。

### 6.5 一貫性の期待値をどう伝えるか

p-fs は eventually consistent だが、`vfs.h` の API は POSIX 風で**同期的・強整合に
見える**。`vfs_write` が返った直後に別ノードの `vfs_read` が古い ref を見るのは
正しい挙動だが、アプリ作者を驚かせる。API レベルでどう「これは eventual だ」を
表現するか（明示的な `pfs_sync()` バリア？ version を握って読む？）は設計の宿題。

---

## 7. 非目標（embrace すること）

- **POSIX 完全互換は目指さない。** 強整合・線形化可能・即時可視は捨てる。
  `regions.md §7` 同様、非同期と eventual を受け入れる。
- **1台のディスクのメタファに戻さない。** path は人間向けの ref に過ぎず、
  記憶の本体は内容アドレスされた不変 block の海である。
- **完璧な複製均衡より、局所性と自己組織化を優先する**（`regions.md §7`）。
  近い region に密、遠い region に疎。ホットは近く、冗長は遠く。
- **「保存」と「公開」を別の操作に戻さない。** save == publish を不変条件とする。

---

## 付録 A — 一枚の絵（ASCII）

```
   人間名 "report.txt"  ──ref──▶  version3 ──parent──▶ version2 ──▶ version1
   (可変・eventual)               │(不変・append-only DAG)
                                  ▼
                          object manifest = [blkA, blkB, blkC]
                                  │ (不変・内容アドレス)
            ┌─────────────────┬──┴──────────────┐
         blkA=H(..)        blkB=H(..)         blkC=H(..)  (大きい block は符号化)
            │                 │            ┌────┴────┐
            ▼                 ▼          shard d1..dk  parity p1..pm
   ─────────────────────────────────────────────────────────────────
   region0 (密 r_in=3)        region1            region2
   ●─●─●  blkA×3,blkB×3       ●─●  blkC d1..dk   ●─●─●  parity p1..pm
   coord ◀──── global/ (疎 r_out, coordinator 間で ref とパリティ) ────▶ coord
   ─────────────────────────────────────────────────────────────────
   読み: block-id → 一貫性ハッシュで責任 region/ノードを決定的に算出
         外れたら region 内 gossip WANT → なお無ければ coordinator 経由 global
   死に: heal.c が後継を選び、生存コピー or k シャードから再複製/再符号化
         → 「最後の1ノードが生き残れば記憶は残る」(replica.h の約束を block へ)
```
