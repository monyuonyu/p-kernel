# External Peer Review — 2026-06-13（コード全体の通信簿）

> このプロジェクトの作法（実装者≠監査者・falsify して台帳に書く）で回した外部レビュー。
> 指揮役が「脳」（AI 数理）を直接読み、4 つの独立監査エージェントを別々に走らせて
> 暗号/relay・永続化FS・カーネル/ring3 隔離・分散層を精読した結果を一枚に畳む。
>
> **接地の規律**: 🔴 と構造的主張（raft 稼働・swim incarnation 死・kdds 空ループ）は
> レビュー時に file:line を**直接確認済み**。それ以外は監査エージェントの file:line 付き
> 報告で、ランタイム未確認のものは各項に「未確認」と明記する。
> 詳細はここ（review doc）に置き、[gap-ledger](architecture/gap-ledger.md) には
> 行動可能な OPEN 行だけを一行で立てる（AUDIT-SPRAWL を再発させないため）。

最終更新: 2026-06-13 ／ 対象: master `6dedb74`

---

## 0. 総評

思想と実装の一致度は、この規模の個人プロジェクトとして例外的に高い。誇張ではなく
**過小評価する癖**すらある。数理は autograd/libc なしで手導出の Transformer backward が
gradcheck を通り、暗号は自作せず TweetNaCl を逐語移植して KAT で固め、分散の 4/6 ファイルは
"中央なし" を主張でなく分岐条件で守り、クラッシュ安全 FS は epoch fence のアトミック切替まで
実装されている。

その上で監査は **実害級バグ 3・思想との矛盾 1・正直さの過大主張 3** を file:line 付きで挙げる。
これらが見つかること自体が、この免疫系（gap-ledger）が機能している証拠でもある。

---

## 1. 重大度順の所見

凡例: 🔴 実害（具体バグ）· 🟠 設計の穴／思想との乖離 · 🟡 honesty gap（過大主張）

### 🔴 SEC-OOB-DRPC — `dnode_table[dst_node]` の配列外アクセス（確認済み）

`drpc_call(UB dst_node, …)`（`arch/common/drpc.c:419`）が `dnode_table[dst_node]` を
**境界チェックなしで索引**（`:422`, `:482`）。`dst_node` は `UB`(0–255) だが `DNODE_MAX==64`
（`arch/common/include/drpc.h:35`）。`dtk_infer`（`:724`）は `node_id >= DNODE_MAX` を弾くが、
`dtk_cre_tsk`（`:657`）と `dtk_sig_sem`（`:689`、`GOBJ_NODE(gsemid)` が満幅バイトを返す）は
**ガードを通さずに到達**する。node id 64–255 でテーブル外を読み書きするメモリ安全バグ。
- **直し方**: `drpc_call` 先頭（と `dtk_sig_sem`）に `if (dst_node >= DNODE_MAX) return E_PAR;`。
- **closed**: 全 `drpc_call` 到達経路が `< DNODE_MAX` を保証し、自己テストが id≥64 を弾くことを示す。

### 🔴 DUR-SWALLOW — durable-write 失敗の握り潰し（確認済み・"忘れる方舟"に直結）

`pfs_put` は `pfs_ark_put()` / `pfs_dur_write()` の戻り値を**捨てる**
（`arch/common/pfs_block.c`、`_TK_HOSTED_LIBC_` ブロック）。永続バックエンドへの書き込みが
失敗しても `PFS_OK` を返す。さらに FIFO eviction（`:198-221`）が「テーブル内の全ブロックは
ARK ログにもある」前提で in-memory コピーを上書きするため、**書き込みが黙って失敗していれば
eviction でデータが消える**。product-soul の最優先課題「忘れる方舟は方舟ではない」の隠れ水路。
- **直し方**: `pfs_ark_put`/`pfs_dur_write` の rc を `pfs_put` の戻り値へ伝播し、失敗時は
  eviction 不可（または durable 確定まで evict 禁止）にする。
- **closed**: durable-write 失敗注入で `pfs_put` が非 OK を返し、当該ブロックが evict されない自己テスト。

### 🔴 SEC-SIGN-TRUNC — `ed25519_sign` が fail-open で切り詰める（確認済み）

`ed25519_sign`（`arch/common/ed25519.c:549`）は `if (msg_len > ED25519_MAX_MSG)
msg_len = ED25519_MAX_MSG;` として**切り詰めたうえで署名**し、戻り値も `void`。一方
`ed25519_verify`（`:563`）は同条件を `return 0` で**拒否**する。署名者と検証者が「何に
署名したか」で食い違う footgun（今の呼び出しは 32–144B なので未露出）。verify が fail-closed
なら sign も fail-closed にすべき。
- **直し方**: `ed25519_sign` を `int` 化し、`msg_len > ED25519_MAX_MSG` で署名せず失敗を返す。
- **closed**: oversize 入力で sign が失敗を返す KAT。

### 🟠 NOCENTRAL-RAFT — 稼働中の Raft リーダーが "中央なし" テーゼに矛盾（確認済み）

`raft_init()`＋`raft_task` がベアメタル x86（`arch/x86/usermain.c:272-273`）と
aarch64（`arch/aarch64/usermain.c:149-150`）で**起動している**。単一 LEADER を選び、
`spawn_on_leader`（`arch/common/spawn.c:200`）等で特権を持つ＝定義上の中央点。
**救い**: flagship の linux x86_64（`arch/linux/x86_64/usermain.c`）は raft を起動せず
swim/world/lookup/gossip で回る ＝ raft からの**移行の途中で、ベアメタルに消し残している**。
加えて raft 自体が toy（即コミット・log backfill なし）で Raft の Safety も満たさない。
- **直し方（二択）**: ベアメタルからも raft を削除 / または明示的に legacy・optional と銘打つ。
- **closed**: 起動時にリーダーを選ぶビルドが無い（or doc が明示的に legacy 宣言）。

### 🟠 SWIM-INCARN — `incarnation` 機構が死んでいる（確認済み）

`incarnation` は wire に確保され（`swim.h:54`）送信パケットへコピーされる（`swim.c:97`）が、
**実値が一度も代入されず、`gossip_apply` でも比較されない**（常に 0）。これは canonical SWIM が
「自分への古い SUSPECT/DEAD 噂を、より新しい incarnation で論駁する」中核機構そのもの。
無いせいで TTL の残った DEAD 噂がノードを再び殺せる／並べ替えで ALIVE↔SUSPECT 振動が起こりうる。
ヘッダコメント「より新しい incarnation で古い疑惑を上書き」が**未実装の機能を説明している**。
- **直し方**: 自ノード単調 incarnation を持ち、ALIVE 再表明時に +1、`gossip_apply` を
  `(incarnation, state)` のレキシコ順 LWW にする。
- **closed**: 古い DEAD 噂を新 incarnation の ALIVE が打ち消す自己テスト。

### 🟠 ISO-USERPTR — syscall のユーザポインタが未検証（監査報告・台帳未掲載の隔離穴）

`SYS_WRITE`/`SYS_READ`/`writev`/`set_thread_area`/stat 系（`arch/x86/syscall.c:346,366,696,742,683`）
が user `arg` を**カーネル範囲に対して検証せず**デリファレンス。`SYS_READ` は user ポインタが
kernel BSS を指しても**書き込む**（confused-deputy）。ELF ローダも `p_vaddr` を検証せず
セグメントをコピーする（`arch/x86/elf_loader.c:258`）。今は flat identity map で隠れているが
本物の隔離穴で、**honesty ledger に載っていない**＝気づかれていない種類の穴。
- **直し方**: `user_range_ok(tid, ptr, len)` を全ポインタ取り syscall 入口と ELF セグメント配置に。
- **closed**: kernel 範囲を指すポインタ syscall が拒否される自己テスト。

### 🟠 DUR-REFTAB — `pfs_dag` の ref テーブルが唯一 self-verify でない永続状態（監査報告）

`refs_persist`（`arch/common/pfs_dag.c:231-250`）は毎変更で ref 表全体を**非アトミックに**
書き直す。content store は content-addressed で自己検証されるが、ref 表は CRC も無い。
同長で本体が腐った `refs.tab` は magic が通って**ガベージの head/seq でロードされる**
（1 バイト反転で name が存在しない manifest を指す）。永続化ストーリーの最弱リンク。
- **直し方**: ref 表に CRC、書き込みを temp+rename（または ARK へ寄せる）でアトミックに。

### 🟠 TRUST-DRPC — DRPC が完全無認証（監査報告・要・信頼境界の明文化）

`drpc_rx`（`arch/common/drpc.c:351`）は magic/version しか見ない。UDP 7374 に届けば
HEARTBEAT 偽装・`DRPC_REQ→CRE_TSK` で他ノードにタスク生成・16bit `seq` 一致だけの
REPLY 注入が可能。relay が完全認証なのと並んで無防備。
- **直し方（最低）**: 「DRPC は信頼された L2 セグメント前提」をコード／doc に明示。将来は
  relay と同じ HMAC を DRPC にも。

### 🟡 HONEST-REORDER — 「fuzzer が reorder 安全を証明」は過大（監査報告・正直さの問題）

ARK の in-tree self-test の電源喪失モデル（`arch/common/arkfs.c:1431`）は **prefix 切り詰めのみ**で、
reorder も非 prefix drop もしない。`fuzz.py` は単一書き込み内の tear/rot/drop/reorder を見るが、
**`fsync` を跨いだ真の write reorder はテストされていない**。end-to-end の reorder 安全性は
「commit の self-check が reorder を**検出可能にする**」ことに依存。README §1 / arkfs ヘッダの
「reorder/torn/corruption に 0 BUG」は **「reorder 下で安全と証明」でなく「self-check が
reorder を検出可能にする」** と書くのが正確。
- **closed**: 文言修正、または fsync バリア規律を明示的にテストする harness。

### 🟡 KDDS-DELCLUSTER — `kdds_delete_cluster` のハンドル閉じループが no-op（確認済み）

`kdds_delete_cluster`（`arch/common/kdds.c:412-439`）の第2ループは条件（`tidx` 解決）まで
計算するが**本体が空**（コメント「このハンドルが削除対象トピックのものなら閉じる」のみ、
`kdds_handles[h].open = 0` を実行しない）。削除トピックのハンドルが開きっぱなしで残る latent バグ。

### 🟡 その他（doc 化のみ、行は立てない）

- スケール天井は実質 **64**（8bit wire `node_id`、`drpc.h:35`）。"every install = a node"
  （`gossip_learn.c`）の語りは 64 天井と両立しない。254 超は wire 変更が要る（コメントは正直）。
- 出力ヘルパー（`mo_putdec`/`dt_putdec`/`r_putdec`）が moe/dtr/r3 で重複。数理カーネルは
  anti-fork 徹底なのに print 系は散在。`arch/common/fmt.c` 集約候補。
- `swim.c:378` の RTT EWMA が 32bit timer delta の生引き算（単位/ラップ前提が無コメント。
  `world.c`/`lookup.c` の wrap-safe 規律と非対称）。

---

## 2. KILL-CHURN-CRASH について — 根因主張ではなく、確認質問ひとつ

gap-ledger の KILL-CHURN 行は、本レビューより遥かに深い。監査が静的解析で出した
「セマフォ待ちキューの残存ノード」説は、**台帳が既に検討して退けている**
（`knl_ter_tsk` の `knl_wait_cancel` が sem+timer 両キューから victim を外す → バグは
dispatch/`knl_ctxtsk` 経路、生き残った queue node ではない）。よって**根因は主張しない**。

残せるのは一つの確認質問だけ:

> `knl_wait_cancel` が「両キューから外す」のは、kill の瞬間に victim が **TS_WAIT に
> いる場合**に限るのではないか？ foreign-kill（`dproc_kill_by_name` →
> `tk_ter_tsk` → `tk_del_tsk`、`drpc.c:242-246`）が victim を **非 WAIT の瞬間**に
> 殺したとき、`tskque` のリンクは本当に常に外れているか。台帳の NOTE は両キュー除去を
> 前提にしているが、その前提が TS_WAIT 限定なら、wave-45 で「現 reproducer が master で
> 再現しない（24/24）」一方で歴史的 ~42% を生んだ choreography は、ちょうど victim が
> 非 WAIT で殺される並びだった、という仮説と整合する。

→ 既に棚にある次の診断（`knl_wait_release_tmout` で `tskque.prev` がテーブル外なら log）に、
**「kill 時の victim の状態（TS_WAIT か否か）」も一緒に記録**すると、この質問に決着が付く。
これは新しい reproducer を立てる方向（台帳の "next lane must FIRST re-establish a reproducer"）
とも噛み合う。

---

## 3. 褒める（balanced であるために — 監査が確認した強み）

- **数理が本物**（直接確認）: `r3_incontext.c:260` の手導出 backward（attention/LayerNorm/
  FFN+ReLU/residual を解析勾配で、ReLU kink を跨ぐ有限差分を明示除外 `:506`、全パラメータ照合）。
  `dtr_expf` の range reduction とバグ墓碑銘コメント（`dtr.c:91`）。
- **暗号の出し方が成熟**: Ed25519 を書かず TweetNaCl 逐語移植＋改変リスト明記＋RFC8032 KAT
  （正例・バイト一致・改ざん拒否の負例）。relay の **MAC-before-replay 順序**（`relay.c:748`）＋
  定数時間比較（`:186`）。`sign_manifest_verify` の fail-closed 3 条件 AND（`sign.c:167`）。
- **"中央なし" が本物**（4/6 ファイル）: `lookup.c` の HRW は ABI 決定性を実装で保証＋LP64 罠を
  `_Static_assert` で捕捉（このセット最高の出来）。`world.c` は各自ビーコンスロットのみ。
  `kdds.c` は「配信数を数える、試行数ではない（試行を数えるのは静かな嘘）」。
  `gossip_learn.c` の no-central テストは「バイト一致でなく順序非依存」を正しく検証。
- **クラッシュ安全 FS のコアが正しい**: ヘッダ/ペイロード妥当性分離（`arkfs.c:381`）、
  epoch fence コンパクション（`:1129`）、全 read self-verify。
- **ring3 fault-reap と FPU 修正が正しい**: `saved_cs&3` 分岐で死んだ文脈に iret しない、
  FXSAVE を tskid 側テーブルへ。コード内 honesty コメントが模範的。
- **規律が定量で出る**: core コードの TODO/FIXME は 4 個、honest マーカーは 82 個
  ＝負債を TODO に隠さず honest bound として本文に書く文化。CI が生の kill/学習テストの
  PASS タグを grep で強制。

---

## 4. gap-ledger に立てた OPEN 行（このレビュー由来）

| id | sev | 一行 |
|---|---|---|
| SEC-OOB-DRPC | 🔴 | `dnode_table[dst_node]` OOB（id≥64）— `drpc_call`/`dtk_sig_sem` にガード |
| DUR-SWALLOW | 🔴 | `pfs_put` が durable-write 失敗を握り潰し OK 返却 → evict で消えうる |
| SEC-SIGN-TRUNC | 🟠 | `ed25519_sign` が fail-open で切り詰め（verify は fail-closed）|
| NOCENTRAL-RAFT | 🟠 | 稼働中 Raft リーダーがベアメタルで "中央なし" に矛盾 |
| SWIM-INCARN | 🟠 | `incarnation` が代入も比較もされず死んでいる |
| ISO-USERPTR | 🟠 | syscall/ELF のユーザポインタ未検証（confused-deputy）|
| DUR-REFTAB | 🟠 | `pfs_dag` ref 表が CRC も atomic 書きも無い唯一の永続状態 |
| HONEST-REORDER | 🟡 | 「fuzzer が reorder 安全を証明」は過大（in-tree test は prefix のみ）|
| KDDS-DELCLUSTER | 🟡 | `kdds_delete_cluster` のハンドル閉じループが no-op |

詳細・file:line・closed 条件は本ドキュメント §1。各行の "closed" は CI 自己テストで強制可能。
