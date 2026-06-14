# compatibility — 出荷後に進化し続ける群れで、古いノードと新しいノードが分裂しないために

> Status: **design DRAFT**（実装前・commander + mk_pino が叩くための叩き台）。
> 確信を装わない。提案し、未解決を §「open problems」で正直に旗立てし、二読みできる
> 意図は §「mk_pino への確認」で commander に返す。
>
> 位置づけ: mk_pino が**初回リリース直前**に立てた問い ——
> 「一度リリースしたら、互換を保ったまま更新を配る。今の仕組みは後から変えられるのか？」
> 中央のない群れでは**出荷済みノードを回収も強制更新もできない**。一部は永遠に旧版で
> 走る。だから protocol/format/model の進化は「古いノードと新しいノードが**相互運用**
> する（群れを分裂させない）」ように**設計**されなければならない。
>
> 最終更新: 2026-06-13 ／ 関連: living-mind.md（MT_WIRE_VER 進化 0→1→2 の実例）,
> native-student.md（§A.3 versioned arch as p-fs object — 本書 §5 の土台）,
> regions.md（capacity(N)/Path W/Fisher）, signing.md（version-poison の脅威面）,
> persistence.md / p-fs.md（content-id 不変式）, gap-ledger.md（互換 gap の正本行）。

---

## ★ DECISION 2026-06-14（mk_pino）— 凍る核は無い。世代交代で進化する

> mk_pino が本ドラフトを2点で正した。これが**正本**。以下 §0〜§9 は土台（seam の現状・
> cert 設計）として有効だが、「3つを凍らせて v1 で完璧にする」という当初の方針は**棄却**。

**訂正① 「v1で完璧」は無理 → frozen core を捨てる。** ローダも自己フォーマットも wire も
いつか必ず変わる。前提は「**全部、後から変わりうる**」。それでも壊れない作り方は ―― モデルの
ために既に設計した **generational succession（世代交代）**:
- 新世代（新wire/新ローダ/新アーキ）が**旧世代と並んで生まれる**。
- **橋ノード**（両版を喋る）が移行期に旧↔新を繋ぐ。
- 旧世代はやがて**死ぬ**。このシステムは**死を前提**に作ってあるから壊れない ―― 旧ノードが
  届かなくなっても、知識は集合に（Path E）、アイデンティティは lineage（Self層のハッシュ鎖）に残る。
- 唯一の現実的な不変量は「凍った形式」ではなく「**鎖**」: **各版が一つ前の版を読める**
  （pairwise migration）。だから常に前へ歩ける。**bedrock は無い、途切れない鎖があるだけ。**

**訂正② 知識/重みの伝播はもう在る（Path E）。新機能ではない。** 二つを峻別する:
- **知識（教えられた内容）= 既存（Path E）。** しかも ―― **アーキが変わっても重み変換は不要。
  新アーキのノードは"赤子"として集合から学び直す。** 知識は重みコピーではなく**再教育**で渡る。
  ＝ ②（アーキ変更）が ①（赤子が学ぶ）に還元される。**from-baby がそのまま移行機構になる。**
- **コード/アーキそのものの配布だけが本当に新しい。** 二段に分ける:
  - **ローダ互換の更新（＝mk_pino の「自動更新」）**: 署名ペイロード（重み・小コードモジュール）を
    **メッシュ**で配り、今のローダがホットロード。**署名必須**（乗っ取り防止 ＝ Ed25519/
    SIGN_MANIFEST、コードと重みの来歴にのみ署名・人間の身元には決して署名しない）。新コードは
    selfc germ（wave-31 の隔離境界）でサンドボックス。
  - **ローダ/wire 自体が変わる深い更新**: **プラットフォーム（APK更新, A/B + ロールバック）**
    ＋ 世代交代でフリート移行。

**一行**: 凍るものは無い。**途切れない鎖（各版が前を読む）＋ 世代交代 ＋ 死を前提 ＋
知識は再教育で渡る ＋ 新コードだけ署名OTA（メッシュ＝小更新／プラットフォーム＝深い更新）。**
これで「全部変わりうる」「乗っ取られない」「自動更新」が三立する。mk_pino の死生観
（死を前提・知識は集合・系譜は残る）が、そのまま互換戦略になっている。

**§7 RELEASE-BLOCKING の読み替え**: 「初回で凍らせて完璧に」ではなく ―― 初回 v1 に要るのは
「**未来版に対して致命的に詰まないだけの最小**」だけ: (i) 未知の高い版/追記を hard-drop せず
読み飛ばす（＝鎖の最初の輪）、(ii) 署名検証つきの更新受理路（メッシュ小更新）、(iii) lineage(Self)
と集合知(Path E) の継続。**完璧である必要はない ―― 次の版が前を読めれば、後から幾らでも変えられる。**

---

## 0. 一言で

**今の seam は「不一致なら REJECT/DROP」だ。これが地雷。** 出荷後に v2 を撒くと、
v1 ノードと v2 ノードが**互いを黙って捨て**、群れが v1 島と v2 島に**ハードフォーク**する。
中央がないので後から繋ぎ直せない。

> 直す方向: **reject → negotiate**（受理レンジ + 相互最高版で話す）＋
> **forward-compatible framing**（旧パーサが未知の追記を**読み飛ばす**封筒）＋
> **falsifiable な互換 cert**（v_n ↔ v_{n+1} が mesh-join/teach/persist できることを
> audit gate と同格のゲートにする）。

**しかし最重要なのはこれ**: 直せないのは**初回バイナリの不一致ハンドラ**そのものだ。
v1 が「高い版を見たら hard-drop する」コードで凍ると、未来の v2 はその v1 に**永遠に
届かない**。よって本書の RELEASE-BLOCKING 部分（§7）は「v1 が将来版に対して何をするか」
——**初回リリースに必ず入っていなければならない最小の前向き互換性**だけだ。

---

## 1. 現状の地層（ground truth — コードで確認済み）

### 1.1 version seam は**至る所にある**（先例は十分）

全パケット/blob が magic + version を持つ。確認した定数（抜粋）:

| seam | 定数 | reject 箇所（コードで確認） |
|---|---|---|
| SWIM membership | `SWIM_VERSION=1` | swim.c:285 `magic!=.. \|\| version!=SWIM_VERSION → return` |
| DRPC heartbeat | `DRPC_VERSION=1` | drpc.c:352 同型 return |
| K-DDS pub/sub | `KDDS_VERSION=1` | kdds.c:364 同型 return |
| pmesh | `PMESH_VERSION=1` | — |
| raft | `RAFT_VERSION=1` | raft.c:177 同型 return |
| pfs replication | `PFSR_VERSION=1` | pfs_repl.c:411 `version!=PFSR_VERSION → return` |
| replica | `REPLICA_VERSION=2` | replica.c:282 「v1 は黙って捨てる」 |
| kloader | `KLOAD_VERSION=1` | kloader_task.c:91,257 |
| **mind/teach wire** | `MT_WIRE_VER` **0→1→2 に実際に進化** | r3_incontext.c:2684 不一致は drop+print |
| storage: 重み | `DTR_WBLOB_VER`, `R3_WP_VER` | r3_incontext.c:793 dims/vocab 不一致は reinit |
| storage: 自己 | `LM_SELF_VER=2`, `ARK_PROF_VER=1`, `SIGN_MANIFEST_VER=1`, `GENOME_VER=1`, `RET_BLOB_VER`, `SFS/PFSD/REPLICA` | sign.c:172 等 |

**結論**: 「後で format を変える」は**既に予期されている**。version byte は全 seam にある。
しかも `MT_WIRE_VER` と `REPLICA_VERSION` は**実際に bump 済み**——進化は架空ではない。

### 1.2 だが seam の**政策**が REJECT/DROP（ここが危険）

例外なく `version != CURRENT → return/drop`。**negotiate も graceful-degrade も無い**。
効果は単純で残酷:

```
v1 ノード  --(SWIM v1 pkt)-->  v2 ノード : v2 が version!=2 で DROP → v1 を見失う
v2 ノード  --(SWIM v2 pkt)-->  v1 ノード : v1 が version!=1 で DROP → v2 を見失う
⇒ 双方向 DROP ⇒ 一つの群れが v1 島と v2 島に割れる（ハードフォーク）
```

### 1.3 forward-compatible framing が**無い**

パケットは固定 C struct（例 `SWIM_PKT` 24B packed, `MT_TEACH_PKT` 53B packed）。
**1 フィールド足すと struct サイズが変わり → version bump → 旧パーサは長さ/版で弾く。**
TLV も skip-unknown も reserved-tail も無い。受信は `if (len < sizeof(PKT)) return;` の
**完全一致前提**（drpc.c:349, swim.c:282, kdds.c:361）。

### 1.4 互換 CONTRACT も interop test も**無い**

v_n ↔ v_{n+1} が話せることを確かめる CI が無い。compat/migration の設計文書も無い（本書が最初）。

### 1.5 ただし**良い前例も既にある**（設計はこれを真似る）

- **MT_WIRE_VER の進化（living-mind.md / dtr.h:345-376）**: 0(LEGACY)→1(LANG)→2(VOCAB)。
  フィールドを**追記**しつつ（origin_node, prov_head, vocab_fp）、不一致を **drop+print** で
  **観測可能に**した。「版で分裂する一点を、隠さず print する」という規律は既にプロジェクトの
  価値観。本書はこれを「drop → degrade」に一歩進めるだけ。
- **content-id 不変式（r3_vocab / pfs_id）**: 語彙やデータの「意味」は content-id で全ノードが
  検証する。**スキーマ合意を中央権威でなく content-id でやる**土台が既にある（§5 で多用）。
- **R3_WP の dims/vocab gate（r3_incontext.c:793-801）**: 旧重みを盲目ロードせず reinit。
  **正しい reject もある**（次元が違う重みは混ぜたら壊れる）。本書の方針は「全部受理」ではなく
  「**安全に縮退できるものは縮退、混ぜたら壊れるものは明示拒否して print**」の二分。

---

## 2. reject → negotiate（§問題1）

### 2.1 受理レンジと「相互最高版」

各 seam の各ノードは単一 `CURRENT` でなく**レンジ** `[MIN_SUPPORTED .. CURRENT]` を持つ。
ピアとは**双方が理解できる最高版**で話す（無ければピアの版に**降りる**）。

二つの実装レイヤを分ける:

- **(N) per-message graceful-degrade（軽い・初回必須の最小）**: 受信版 `v` が
  `MIN_SUPPORTED <= v <= CURRENT` なら**その版のパーサで読む**（DROP しない）。
  `v > CURRENT`（未来版）なら**hard-drop しない** ——「未来版が前向き互換封筒（§3）で
  包んだ legacy コア部分」を読む。`v < MIN_SUPPORTED`（化石）なら print して drop（正直に）。
- **(C) capability/min-version handshake（重い・後から足せる）**: SWIM/DRPC の最初の
  接触で `{min_supported, current, cap_bits}` を 1 回交換し、ピアごとに「話す版」を表に持つ。
  これは**最適化**であり初回必須ではない（per-message で十分に分裂は防げる）。
  handshake を入れるなら SWIM gossip に capability byte を**追記**する形（§3 の封筒で）。

> **設計判断**: 初回は **(N) per-message degrade** だけを必須にする。(C) handshake は
> envelope が入っていれば後から非破壊で足せる。一度に両方やらない。

### 2.2 初回バイナリが**今日必ず持つべき**不一致ハンドラ（直せない一点）

v1 の `swim_rx`/`drpc_rx`/`kdds_rx`/`mind_net_task` の版チェックは、出荷後に**凍る**。
だから v1 は将来版に対し、最低限これをしなければならない:

1. **未来版（`v > CURRENT`）を hard-drop しない。** legacy コア（封筒の固定先頭部分）を
   読み、membership/heartbeat/最低限の teach だけでも成立させる。
2. **未知の追記 tail を読み飛ばす**（§3 の skip-unknown）。
3. **不一致を print する**（MT_WIRE_VER 同様、分裂点を観測可能に）。

これが入っていない v1 を出荷すると、**未来の互換性は物理的に不可能**になる。§7 で
RELEASE-BLOCKING 化する最小はまさにこれ。

### 2.3 「降りる」の意味（degrade の具体）

- membership/heartbeat（SWIM/DRPC）: 版が違っても**生存情報だけは必ず通す**。新フィールドが
  読めなくても「このノードは生きている」は読める ——これが島分裂を防ぐ生命線。
- teach（MT wire）: §5 と絡む。vocab/arch が**実際に違う**なら混ぜると壊れる（content-id
  gate は正しい拒否）。だが「同じ vocab・新しい任意メタ付き」なら**メタを捨てて core teach を
  受ける**。「拒否」と「縮退」を content-id で分ける（§5.3）。

---

## 3. forward-compatible framing（§問題2）— 封筒 + 追記 tail

### 3.1 封筒（versioned envelope）

全ネットワークメッセージを共通の薄い封筒で包む。**固定先頭**（旧パーサが必ず読める）+
**legacy struct そのまま**（再利用）+ **長さ前置きの型付き拡張領域**（旧は読み飛ばす）。

```
+------------------------------------------------------------------+
| ENVELOPE HEADER (fixed, frozen forever in v1)                    |
|   u32 magic         seam ごと（SWIM_MAGIC 等、既存を流用）        |
|   u8  env_ver       封筒フォーマット版（= 1 で凍結）             |
|   u8  proto_ver     中身プロトコル版（既存 SWIM_VERSION 等）     |
|   u16 core_len      legacy core の長さ（旧 struct sizeof）        |
|   u16 ext_len       拡張領域の総バイト（0 = 拡張なし）           |
|   u16 _rsv          予約（0）                                     |
+------------------------------------------------------------------+
| LEGACY CORE  (= 既存の SWIM_PKT / MT_TEACH_PKT を bit-for-bit)   |  ← 旧コードはここだけ読めば動く
+------------------------------------------------------------------+
| EXT AREA  (ext_len bytes) : 0個以上の TLV                        |  ← 旧コードは ext_len 分 skip
|   各 TLV = { u16 type, u16 len, u8 val[len] }                    |
|   skip-unknown 規則: 知らない type は len 分読み飛ばす           |
+------------------------------------------------------------------+
```

**skip-unknown 規則（前向き互換の心臓）**: パーサは `core_len` 分の legacy core を読んだ後、
`ext_len` の範囲で TLV を回す。**知らない `type` は `len` 分スキップ**して次へ。これで
新版が足した OPTIONAL フィールドを旧版が**安全に無視**できる。

**重要な制約**: 新フィールドは**必ず OPTIONAL**。「これが無いと意味が壊れる」必須フィールドを
ext に置いてはいけない（旧版が skip するから）。必須の意味変更は version bump + degrade-or-refuse
（§2.3 / §5）の領分。

### 3.2 既存固定 struct の最小レトロフィット

`MT_TEACH_PKT` を例にすると（53B packed）:

- **core = 現 `MT_TEACH_PKT` をそのまま**（vocab_fp まで含め bit 互換）。`core_len=53`。
- envelope header（12B）を**前置**。`magic` は MT_MAGIC を envelope magic に昇格（衝突なし、
  既に先頭 u32）。
- 新メタ（例: 教師の region hint, 信頼スコア）は ext TLV へ。旧 `mind_net_task` は core 53B を
  読み、ext を skip。

**コスト現実**: これを **~8 protocol すべて**でやるのは小さくない（§open problems #2）。
だが**全部一度にやる必要はない**。生命線（SWIM/DRPC membership）の封筒化だけが §7 で
release-blocking。残りは「TLV を後で足したくなった時」に封筒を被せれば良い ——
**ただし封筒を被せること自体が version bump** なので、その時旧ノードが degrade できる
ように **per-message degrade（§2.2）が初回に入っている**ことが前提。だから順序は
「degrade ハンドラ（必須）→ 封筒（生命線のみ必須、他は随時）」。

### 3.3 もっと安いレトロフィット（封筒なしで前向き互換を作る裏技）

封筒を全 seam に被せるのが重いなら、**最も安い前向き互換**はこれ:
受信側を `if (len < sizeof(PKT)) return;` から `if (len < sizeof(PKT)) return;`（最小長
チェックは残す）に変えつつ、**`len > sizeof(PKT)` を拒否しない**（末尾の余剰を無視する）こと。
これだけで「新版が struct 末尾に追記 → 旧版が余剰を無視して core を読む」が成立する
（reserved-tail 方式）。**封筒 TLV より弱い（型なし・位置依存）が、初回バイナリに入れる
コストは数行**。§7 の cheapest first step はこれを軸にする。

---

## 4. 互換 CONTRACT + falsifiable interop cert（§問題3）

### 4.1 契約（the rule）

> **版 N のノードは、N-1 と N+1 のノードと相互運用しなければならない**
> ——mesh join できる・teach が通る・persist round-trip（旧 blob を読める）。
> さらに（長い尾、§6）**版 N は「N より十分古い化石」とも、membership だけは成立する**
> （生存情報は永遠に通す）。

content-id で「混ぜたら壊れる」と判定したものは**この契約の例外**として明示拒否して良い
（§5.3）。契約は「黙って分裂しない」であって「何でも混ぜる」ではない。

### 4.2 falsifiable interop cert（audit gate と同格）

CI が**二つの版をビルドして実際に話させる**。`gl_merge` 系の数値 cert と同じ「自分で
反証する」流儀。cert タグ:

| タグ | 確かめること | 反証（FAIL）になる条件 |
|---|---|---|
| `[compat-mesh]` | v_old と v_new を 1 クラスタに入れ、互いを ALIVE と見る | どちらかが相手を DROP して島になる |
| `[compat-teach]` | v_old が teach → v_new が同じ答えを返す（同 vocab） | 版差で teach が落ちて答えが出ない |
| `[compat-persist-read-old]` | v_new が v_old の書いた blob を読んで upgrade | v_new が旧 blob を reject/破棄 |
| `[compat-degrade]` | v_old が「未来版パケット」を受けても crash/drop せず core を読む | hard-drop or panic |
| `[compat-skip-unknown]` | 既知 core + 未知 TLV を旧パーサが core だけ読む | 未知 type で誤読/拒否 |

**ビルド two-versions の作り方（設計）**: `git worktree` で `proto_ver` 違いの二バイナリを
linux boot で立て、relay/ループバックで同一クラスタに入れる。`MT_WIRE_VER` の
`[lang-wire-verdrop]` / `[lang-vocab-refuse]` cert が既にこの「版差を print で観測する」
パターンの**生きた前例**——それを「分裂する」確認から「**分裂しない**」確認へ反転させる。

### 4.3 gate 化

`[compat-mesh]` と `[compat-degrade]` を **release gate**（master へ入る条件）にする。
implementer≠auditor の規律どおり、auditor が two-version harness を**作る**（commander は
ゲート式を行ごとに読む——validator-trap の教訓）。

---

## 5. persisted-format migration（§問題4）& model-format evolution（§問題5・本丸）

### 5.1 永続 blob は READ-OLD→UPGRADE（reject しない）

新コードは version-tagged 旧 blob（`DTR_WBLOB_VER`, `LM_SELF_VER`, `ARK_PROF_VER`, …）を
**読んで昇格**する。`switch(blob->version){ case 1: …; case 2: …; }` で各旧版から現行 struct を
構成。**データを捨てない**規則:

- 読めた旧版は**必ず現行へ翻訳して保持**（欠けた新フィールドはデフォルトで埋める）。
- 翻訳不能（content が本質的に非互換、例 dims 不一致の生重み）だけが拒否——だが**print して
  理由を残す**（R3_WP の前例どおり）。「黙って消す」は禁止。
- **書き戻しは慎重に**: 旧版を読んで新版で**上書き保存すると、旧版ノードがもう読めなくなる**
  （片方向 upgrade のロックイン）。混在期は「読めるが**最古サポート版で書く**」or「両版併存
  （content-addressed なら別 object）」を seam ごとに選ぶ。これは §open problems #4。

### 5.2 model-format は**絶え間なく変わる**（native-student の成長）

native-student.md §A.3-A.5 が既に設計している: 生徒は**生きたまま arch を大きくする**
（expert を足す/幅/深さ/vocab 成長＝Evolution 層）。**だからモデル形式は定常的に変わる。**
これが互換の**最も鋭い面**——SWIM の版差は「membership が通れば許せる」が、モデルは
「次元が 1 違えば数値が壊れる」。

### 5.3 成長するモデルが群れ全体で互換であり続ける設計

native-student.md §A.3 の **arch-spec を p-fs object 化**を土台に、互換の観点を足す:

1. **versioned arch-spec as p-fs object**: `{E, top-k, D, n_layers, vocab_id, kernel_flags}`
   を content-addressed object に。重み blob は arch-spec を**参照**。**arch の同一性 =
   content-id の一致**（§1.5 の content-id 不変式の再利用）。中央権威は要らない。
2. **degrade-or-refuse を content-id で判定（§2.3 の心臓）**: teach/weight を受けたら、
   付随 arch-spec id を自分のと照合。
   - **同一 id**: 完全互換、そのまま merge（Path W / Fisher, regions.md）。
   - **既知の祖先/子孫 id**（系譜で繋がる）: §5.4 の翻訳を試みる。
   - **無関係 id**: 混ぜると壊れる → **REFUSE + print**（vocab_fp gate の一般化）。
     これは契約違反ではない——「黙って分裂」しないために**観測可能に**拒否している。
3. **concurrent old/new during rolling migration**: 成長は「新 arch-spec 発行 → 旧重みを
   §A.4 の function-preserving 翻訳で新 object へ → 両者併走 → 新が no-regress を満たしたら
   旧を退役」。SWIM/capacity(N) の再シャード既存路に乗る（native-student §A.3）。
4. **old-model-teaches-successor（generational distillation）**: 旧 arch のノードは新 arch を
   直接 merge できなくても、**soft-target を teach** できる（蒸留は arch 非依存——出力分布の
   一致だけ見る）。これが「化石ノードが**部分的に有用であり続ける**」(§6) 道。旧世代が
   新世代を**育てる**——native-student の distill-and-diffuse がそのまま世代間互換の橋。
5. **decentralized schema agreement（no gatekeeper）**: 「今どの arch が active か」を
   **誰も宣言しない**。各ノードが自分の arch-spec id を gossip し、region 内で**最も普及した
   id（または最も多くの重みを持つ系譜）へ eventual に収束**。raft の強合意は**要らない**
   （§open problems #3 でこの賭けを正直に検証する）。これはプロジェクトの no-gatekeeper
   哲学と一致——format provenance（license/content-id）は検証するが、**人間の身元は決して
   検証しない**（ark の不変式）。

### 5.4 世代間の重み翻訳（function-preserving、native-student §A.4 を互換側から）

旧（E=4）→新（E=8）の翻訳が**出力をほぼ保つ**なら（Net2Net 風 expert 複製 + router 拡張、
native-student §A.4）、`[grow-preserves]` cert（成長前後で固定プロンプト出力が ε 以内）が
**そのまま互換 cert**になる。「成長しても赤ちゃんが急に別人にならない」= 「新 arch が旧 arch と
意味的に互換」。**成長の cert と互換の cert は同じもの**——ここは設計が綺麗に閉じる。

---

## 6. app update distribution & 永遠の長い尾（§問題6・正直に）

- **今日は手動 sideload（ADB）のみ。** 本物の更新には channel が要る: Play Store /
  F-Droid / 自前 self-update。どれも**強制更新はできない**——ユーザが更新しなければ旧版で走る。
- **永遠の長い尾を設計前提にする。** N±1 だけでなく「**ずっと古い化石ノード**」が常に居る。
  契約（§4.1）は二段:
  - **強い保証（N±1）**: mesh + teach + persist がフル互換。
  - **弱い保証（化石〜N）**: **membership だけは永遠に通す**（生存情報＝封筒の固定先頭、
    §3.1 の凍結ヘッダ）。teach/merge は content-id 次第で degrade or refuse。
    化石は「数を数える・生存を伝える・soft-target で次世代を教える（§5.4-4）」程度には
    **常に有用**であり続ける——「ancient node still partially useful」。
- **だから封筒の固定先頭（magic + env_ver + proto_ver + core_len + ext_len）は永久に凍結**。
  これが「未来の任意の版」と「過去の任意の版」が最低限握手できる唯一の不変点。env_ver を
  上げる時は**最大限の慎重さ**（封筒自体の互換は最後の砦）。

---

## 7. RELEASE-BLOCKING vs later、そして最も安い第一歩

### 7.1 RELEASE-BLOCKING（初回公開バイナリに**必ず**入る——出荷後に凍るから）

これらは「v1 が未来版に対して何をするか」で、**後から直せない**:

1. **未来版を hard-drop しない（§2.2）**: 全ネット seam の `version != CURRENT → return` を
   「`v > CURRENT` は core を読む / `v < MIN` は print して drop」に変える。**最小で最重要。**
2. **末尾余剰を拒否しない（§3.3 reserved-tail）**: `len > sizeof(PKT)` で drop しない。
   数行。これが「未来版が末尾に追記」を旧版が飲める唯一の保証。
3. **membership は版差を越えて必ず通す（§2.3 / §6）**: SWIM/DRPC の生存情報は版が違っても
   ALIVE を立てる。島分裂の生命線。
4. **封筒の固定先頭を凍結定義（§3.1）**: 今 envelope header の**バイト並びだけ**確定して
   おく（中身 TLV は後で良い）。これを後で変えると過去ノードと握手できなくなる。

### 7.2 later でよい（封筒が前提を満たしていれば非破壊で足せる）

- capability handshake (§2.1-C)、全 seam の TLV 化（§3.2）、persist write-back 戦略の精緻化
  （§5.1）、generational distillation の実装（§5.4）、interop cert harness の全 seam 展開、
  arch-spec eventual-consistency の収束則（§5.3-5）。
- これらが later で済むのは「§7.1 の前向き互換ハンドラが初回に在れば、後の追記を旧版が
  飲める」から。**§7.1 が無いと later が全部 release-blocking に昇格する**——ここが mk_pino の
  問いの核心。

### 7.3 単一・最も安い決定的第一歩

> **全ネット受信ハンドラの版チェックを「reject」から「accept-known / read-future-core /
> print-fossil」に変え、`len > sizeof(PKT)` を許容する（§7.1-1,2 + §3.3）。**
> 加えて `[compat-mesh]` + `[compat-degrade]` の two-version harness を一つ書いて gate 化。

理由: これは**数十行**（8 seam の rx 各 1 箇所 + 最小長チェック緩和）で、**初回に在れば
未来が開き、無ければ未来が閉じる**唯一の不可逆点を押さえる。封筒 TLV・handshake・
schema 収束は全部この後に非破壊で積める。MT_WIRE_VER の drop+print 前例があるので、
「版差を観測可能にする」文化と harness の骨格は既にある——反転させるだけ。

---

## 8. open problems（容赦なく正直に）

1. **ハードフォーク・リスク（最大）**: §7.1 が初回に入らなければ、v2 を撒いた瞬間に
   群れが割れ、中央がないので**二度と繋がらない**。これは「後で直す」が物理的に効かない
   唯一クラスの問題。**初回が事実上の憲法**。
2. **封筒レトロフィットの工数**: ~8 protocol の固定 struct を envelope 化するのは小さくない。
   緩和策は §3.3（reserved-tail で安く前向き互換だけ先に取る）+ §7.2（封筒は随時）だが、
   「安い前向き互換（型なし末尾）」と「正しい TLV（型付き）」のどちらを初回に取るかは
   **二読みできる**——commander/mk_pino 判断（§mk_pino への確認 Q1）。
3. **schema 合意に強合意（raft）は要るか、gossip/eventual で足りるか**: §5.3-5 は「raft 不要・
   eventual 収束」に賭けているが**未証明**。複数 arch が同時普及して**収束しない/振動する**
   可能性（survival-network の二層振動と同型のリスク）。raft は存在するが、no-gatekeeper
   哲学と「誰も active schema を宣言しない」が緊張する。最小実験で反証すべき賭け。
4. **persist 片方向 upgrade ロックイン**: 旧読んで新で書き戻すと旧版がもう読めない（§5.1）。
   混在期に「最古サポート版で書く」か「両版併存」かは seam ごとに違い、一般解が無い。
5. **security: version/capability poison**: 悪意ノードが「自分は超古い/超新しい」と詐称、
   または毒 capability bit を広告して degrade を**強制**（downgrade 攻撃）し得る。
   signing.md の manifest 署名は**コード/重みの来歴**を守るが、**ネゴシエーションの版広告**は
   今は署名対象外。「最低版を偽って相手を弱い経路に落とす」古典的 downgrade 攻撃面が新設される。
   ——ただし ark 哲学上**人間の身元は検証しない**ので、防御は「版広告そのものの整合性」
   （署名された arch-spec id・content-id 検証）に閉じるべきで、ノードの身元認証に**してはいけない**。
6. **genuinely unsolved vs engineering**:
   - **engineering**（やれば済む）: reject→degrade、reserved-tail/封筒、persist read-old、
     interop cert harness、generational distill 実装。
   - **frontier（未踏）**: 中央なしの **schema eventual-consistency の収束保証**（#3）、
     downgrade 攻撃を身元認証なしで防ぐ版広告の真正性（#5）、世代間 vocab 整合の蒸留（§5.4 +
     native-student §A.5 の honest gap）。ここは「最小の決定的実験で賭けを検証してから広げる」
     ——native-student と同じ規律。

---

## 9. mk_pino への確認（二読みできる意図）

- **Q1（封筒の重さ）**: 初回に「型付き TLV 封筒」まで入れるか、それとも「reserved-tail で
  安く前向き互換だけ」取り、封筒は随時にするか。本書の推奨は**後者**（§7.3）——だが
  「format を後で**確実に**変えたい」が強い要求なら、生命線 seam だけは初回 TLV 封筒もあり。
- **Q2（schema 合意の哲学）**: §5.3-5 の「誰も active arch を宣言しない・eventual 収束」は
  no-gatekeeper 哲学そのものだが、**収束しないリスク**を負う。raft（中央寄り）を schema 合意に
  **だけ**使う妥協を許容するか、それとも「振動してでも宣言者を置かない」を貫くか。これは
  技術判断であると同時に**哲学判断**——「可視化＝仕組み」の流儀で、収束状態を群れが
  **観測できる**仕組み（どの arch が今どれだけ普及しているかの分散マップ）を併せて要るか。
- **Q3（化石の有用性の下限）**: §6 の「化石は membership + soft-target teach までは永遠に
  有用」で十分か、それとも「化石も最低限 ask に答えられる」までを契約に含めたいか。後者は
  全モデル版を化石でも実行可能に保つ重い制約になる。
- **Q4（強制更新の不在をどう語るか）**: 「永遠に旧版が居る」は bug でなく**設計前提**
  （native-student の「遅く成ること自体が点」と同じ思想——多様な版が共存する群れ）。
  これを product として**前向きに**語るか（地層・多様性）、リスクとして語るか。

---

## 10. この設計が既存とどう噛み合うか（一望）

- **MT_WIRE_VER（living-mind）**: 「版差を drop+print で観測可能に」を「degrade して
  分裂させない」へ反転。前例の文化と cert 骨格を継承。
- **native-student §A.3-A.5**: arch-spec の p-fs object 化・function-preserving 成長翻訳を
  **互換の土台**として再利用。`[grow-preserves]` = 互換 cert（§5.4）。
- **regions.md（Path W / Fisher / capacity(N)）**: 重み merge と rolling re-shard の既存路に
  世代併走を乗せる。
- **signing.md**: 版広告の真正性（§8-5）は署名された content-id に閉じる——人間の身元は検証しない。
- **gap-ledger.md**: 「互換 CONTRACT 無し / interop cert 無し」を**新規 OPEN 行**として
  起票し、`[compat-mesh]`/`[compat-degrade]` gate の PASS で閉じていく（行が減る＝進捗）。
