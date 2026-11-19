# 方舟の入口 v1（1人でも使える）の設計メモ
<!-- vmodel rung: OPEN -->
<!-- vmodel certs: - -->

2026-11-05。ROADMAP「デモの次の山」、全体の決まり 11（原本は消さない）・12（継ぐ人）、inbox #38 の 5。docs だけ、実装は 12 月から。監査-131〜133 の差し戻しで直した。

## 1. 今あるもの（コードで確かめた）

- `pfs_dag_save`（名前→版の鎖、region へ複製）。**1 版 4096 バイトまで**（`PFS_E_TOOBIG`）、**名前は 16 まで**（`PFS_REF_MAX`）、**`pfs_dag_read_at` は頭から 64 版までしかさかのぼれない**（`PFSD_LOG_MAX`）。置き場は ARK・平ファイル・メモリだけ（ベア、64 ブロックで満杯なら断る）
- `ark_compact` は索引の全部のブロックを残す（11-07 の merge 84e58cda から。前は生きたファイルと木だけ＝p-fs のブロックが消えた、実測 `pk-scratch/a132/compact_probe.c`）。製品から呼ぶのは hosted の `pfs_ark_put`（11-07 feat/ark-autocompact）: ログが像の半分を超えたら put の前に、満杯なら一度、詰め直す（新しいログは空きに全ブロックの写しを書く＝生きたブロックは像の約半分まで）。`tests/host/run_ark_full.sh`（64 セクタに 14 件・128 に 28 件、前は 10・21）
- 消す口: `pfs_drop`（mind の重みの GC が長さと頭 4 バイトの `MW_MAGIC` で選んで落とす）
- 同意は ark-profile v1、鍵はノードの Ed25519（人の身元ではない）。暗号化は無い

## 2. 決めたこと（分かれ道はおすすめで決めた）

1. **記録ごとに名前なしの版の鎖**: `ARK_REC`（magic・版・前の版の id・時刻・種類・見せ方・学習の同意・長さ・断片の id）を `pfs_repl_put` で置き、前の版を id で指す。本文は 3.5KB の断片（頭に平文の `ARK_FRAG` の magic＝`MW_MAGIC` と重ならない）。**`ark/rec` は目次だけ**（各記録の最新の版の id の木、1 ブロック 127 個。gossip_learn の形は 2 段＝16129 件で止まるので、段の数を目次の頭に持ち、満ちたら古い根を新しい根の 0 番にして段を足す＝記録の位置は保つ）＝64 版の上限は要らない昔の目次にだけ掛かる。直すのは版を足すことだけ。**書く順は 断片→`ARK_REC`→目次の葉→根→`ark/rec` の版**（途中で落ちても前の目次は全部読め、新しい版が目次に載らないだけ）。限界: 詰め直しは全部残すので、直すたびに目次の段の数だけ 4KB が残る
2. **v1 の置き場は ARK だけ**
3. **詰め直しは p-fs のブロックを残す（11-07）。製品から `ark_compact` を呼ぶ道は `pfs_ark_put`（11-07、§1）**
4. **見せ方は記録ごと、既定は「自分だけ」**（誰にでも／継ぐ人に／自分だけ）
5. **公開でない記録はノードを出る前に暗号に**（X25519＋ChaCha20-Poly1305、既知の答えの試験つき）。記録の鍵は本人に包む。**「継ぐ人に」は継ぐ人へ直接は包まない**（決まり 12 の待ちの期間の前に読める）＝書いた時点で R-ARKKEY のおすすめ（region の n 台に k-of-n）で預け、渡す処理（待ちの期間・取り消し）は v1.1。限界: k 台が示し合わせれば早く読める。「自分だけ」の鍵はどこにも預けない。答えが違えば実装の前に直す
6. **学習の同意は記録ごと**（既定は使わない）
7. **継ぐ人の指名は最初の版から**（公開鍵を `ark/heir` に、身元は確かめない。名前は 11-09 に `self/heir` から変えた、§4）

## 3. 合否（実装より先に固定、壊したら赤）

- E1 記録が再起動と region の 1 台の kill を越えて全バイト同じ。10KB の記録も、**記録 100 件と 1 件の 70 版も**、目次が 3 段になる数も（試験では 1 ブロックの数を小さくして）。目次の版を書く前に止めても前の版が全部読める
- E2 新しい版で古い版が読める（`-DARK_REC_OVERWRITE` で赤）。`mw_gc` と `ark_compact` の後も
- E3 公開でない記録に本文の印が出ない（`-DARK_REC_PLAIN` で赤）。継ぐ人の鍵だけでも k−1 台の分でも開かない
- E4 見せ方の既定は自分だけ。継ぐ人の追加と取り消しが版で残る
- E5 同意しない記録が cradle に入らない（`-DARK_REC_LEARN_ALL` で赤）
- E6 入口にいいね・数・順位が無い

## 3b. 暗号の部品（設計、2026-11-08。原文は `pk-scratch/rfc/` の RFC 7748・8439・4231）

hosted だけ（v1 の置き場が hosted）。新しいファイル `arch/common/ark_crypto.c` と `ark_crypto.h`。
- **X25519**（RFC 7748 §5）: ed25519.c の体の演算（TweetNaCl 由来の gf・M・S・inv25519・sel25519）をそのまま使う＝はしごの関数を ed25519.c の中の `#ifdef _TK_HOSTED_LIBC_` に置く（ベアの機械語は変わらない＝crown を測る）。鍵の整え方（先頭の下 3 ビットを 0、末尾の最上位を 0・次を 1）と、受けた u の最上位ビットを落とす（MUST）。共有の値が全部 0 なら断る（§6.1、小さい位数の点）
- **ChaCha20・Poly1305・AEAD**（RFC 8439 §2.3〜2.8）: ark_crypto.c に一から。開くときは tag を一定時間で比べ、合わなければ平文を 1 バイトも返さない（出力を 0 で埋めて −1）
- **鍵の導き**: HKDF の形（抽出と展開）を HMAC-SHA-512 で（ed25519_sha512 の上に）。入れるのは共有の値 K と両方の公開鍵（§6.1「K・K_A・K_B を含む KDF」）
- **包み方**: 記録の鍵は記録ごとにでたらめ 32 B。本人へ包むときは毎回使い捨ての X25519 鍵を作り、導いた鍵で記録の鍵を AEAD で包む（使い捨ての公開鍵を隣に置く）。**どの鍵も 1 回しか暗号に使わない＝nonce は 0 で固定**（数えの状態を持たない。同じ鍵で 2 回目を呼んだら断る試験を置く）。でたらめは getrandom（失敗なら断る、時刻などで埋めない）

### 合否（実装より先に固定）
- C1 X25519: §5.2 の 2 組・§5.2 の繰り返し 1 回と 1000 回・§6.1 の Alice と Bob（双方の公開鍵と共有の値）が全バイト同じ
- C2 ChaCha20 §2.3.2・§2.4.2、Poly1305 §2.5.2 と付録 A.3、鍵の生成 §2.6.2、AEAD §2.8.2（封と開）と付録 A.5（開）が全バイト同じ
- C3 HMAC-SHA-512 が RFC 4231 §4.2〜4.8 と同じ（§4.6 は頭の 128 ビット）
- C4 tag・AAD・暗号文の 1 ビットを変えると開かず、出力は全部 0。u＝0 の共有は断る
- 陰性: `-DARKC_NC_NO_CLAMP`（C1 赤）、`-DARKC_NC_TAG_NOCHECK`（C4 赤）、`-DARKC_NC_NO_PAD`（AEAD の詰め物なし、C2 赤）、`-DARKC_NC_NO_MASK`（u の最上位ビットを落とさない。§5.2 の 1 組目の u は末尾 0x4c で最上位が 0＝効かず、2 組目は 0x93＝赤のはず。TweetNaCl の unpack25519 は 0x7fff で落としているので、陰性はその X25519 の道だけで外す）
- 試験はシェルの `ark crypto`（UMP x86_64 に数秒）。限界: 時間の側の漏れ（キャッシュ・分岐）は試験しない、乱数の質は試験しない
- 状態（2026-11-08、**第 1〜3 段の前半は監査-161 PASS**＝RFC を取り直して生成し直したヘッダは全桁同じ、OpenSSL と乱数の入力 14,140 件で全部一致、ベアの .text も全桁同じ。限界: 乱数の入力では最後の簡約の抜けは見えない＝A.3 だけが捕まえる）: 第 1 段＝ChaCha20・Poly1305・AEAD（feat/ark-crypto-1）。C2 のうち §2.4.2・2.5.2・2.6.2・2.8.2（封と開）・付録 A.3 の 11 本・A.5（開）と C4 の改ざん、陰性 NO_PAD・TAG_NOCHECK。試験はまだシェルでなく tests/host/run_ark_crypto.sh（ASan・UBSan つき、UMP x86_64 に 3 秒）。最後の簡約や繰り上がりを外す壊しは A.3 でだけ赤（2.x の値では緑）。第 2 段＝X25519（feat/ark-crypto-2、監査待ち）: C1 の §5.2 の 2 組・繰り返し 1 回と 1000 回・§6.1、u＝0 と u＝1 は断る、陰性 NO_CLAMP は 4 行・NO_MASK は §5.2 の行だけ赤。ed25519.c の末尾の hosted の塊（ベアの .text は測って同じ）。第 3 段の前半＝HMAC-SHA-512（feat/ark-crypto-3、監査待ち）: RFC 4231 の 7 例（§4.4 は本文に「=」が抜けている＝生成スクリプトで吸収）。第 3 段の後半＝HKDF-SHA-512（監査-162 PASS: Wycheproof の JSON を取り直して sha256 と生成し直したヘッダが全桁同じ）: RFC 5869 に SHA-512 の値が無いので Wycheproof の hkdf_sha512_test.json（80 有効・長すぎ 3）から生成、陰性 HKDF_NO_CHAIN・HKDF_NO_LIMIT。第 4 段＝鍵の箱（監査-162 限界つき PASS: K4 の生成し直しは全桁同じ。空の箱の封・開・包みを断る行を監査で足した。getrandom の失敗の道は試験に無い＝test-holes H18）: 下の K1〜K4 が 7 行で緑、BOX_REUSE は K1・K2 だけ赤（下の部品の陰性も箱の行を赤にする）。`ark crypto`（11-08 feat/ark-crypto-shell。監査-164 FAIL＝Android の CMake に無く check_parity が赤、CI の段は前の段の make clean で p-kernel が無い→直して**監査-165 PASS（限界つき: Android の実ビルドは未確認＝glibc の見出しで道の分かれだけ模擬、CI の APK のジョブが初めての判定）**）: カーネルの中で 5 行（aead・tamper・x25519・wrap の K4 と戻した箱の封を断る・box と 2 つの箱の鍵が違う）、hosted の x86_64・aarch64・Android（API 28 未満は /dev/urandom）。Windows は「不可」の 1 行を返し、表も不可。ベアは表で不可（getrandom）。陰性 NO_CLAMP は x25519・wrap、TAG_NOCHECK は tamper だけ赤。全部の値の試験は run_ark_crypto.sh のまま。残り: 記録と目次への組み込み
- 監査-158 の注（実装の前に決めること）: 「数えの状態を持たない」と「2 回目を断る」は両立しない＝鍵は使い捨ての鍵の箱（使ったら印・鍵を 0 で消す）で持ち、断るのはその印。**記録を直して暗号にし直すときは記録の鍵も作り直す**（同じ鍵・nonce 0 で 2 つ目の平文を封じると、ChaCha20 の流れが重なり 2 つの平文の XOR が漏れる）。これを陰性（直した記録を同じ鍵で封じる）で赤にする試験を C4 の隣に
- **鍵の箱（第 4 段、2026-11-08 に決めた）**: 箱＝記録の鍵 32 B と「封じた」印。封は 1 回だけ、開くのは何度でも。**包みから戻した箱は生まれつき「封じた」**＝保存から読んだ鍵では開くことしかできない（数えの状態を持たずに 2 回目を断れる）。包み＝使い捨ての X25519 鍵 e、K＝X25519(e, 相手の公開鍵)、包む鍵＝HKDF-SHA-512(salt＝e の公開鍵‖相手の公開鍵、ikm＝K、info＝"p-kernel ark wrap v1")、AEAD・nonce 0・AAD なし。形は e の公開鍵 32‖暗号の鍵 32‖tag 16＝80 B。合否 K1 2 回目の封は断る（暗号文は 0）K2 戻した箱の封は断る・開くのは通る K3 違う秘密鍵・改ざんでは戻らず箱は空 K4 決めた e の包みが別の実装（python の cryptography）と全バイト同じ。陰性 `-DARKC_NC_BOX_REUSE`（K1・K2 赤）

## 4. 順番

詰め直しの直し → 暗号の部品 → 記録と目次（E1・E2・E4）→ 暗号で複製と預け（E3）→ 学習の印（E5）→ 画面（E6）→ v1.1（渡す処理）

- 記録と目次（2026-11-09、feat/ark-rec、**監査-169 限界つき PASS で master**。監査の壊し: 根を足すときの段の取り違え・見せ方の版で本文を作り直す・頭の fanout を無視する は赤、送り止めだけ外すのは緑＝test-holes H22）: `arch/common/ark_rec.c`（hosted だけ）とシェルの `ark rec`・`ark heir`、試験 `tests/host/run_ark_rec.sh`（E1 の再起動・E1c 目次の版の前で止める・E2 詰め直しの後・E4・2 台の local）。**名前は `self/rec`・`self/heir` でなく `ark/rec`・`ark/heir`、E3 までは方舟の物をノードから出さない**: 名前（ref）は region 全体で共有され新しい seq が勝つので、2 台の目次が食い合う（探り: B が A の目次を取り込み、自分の記録を書けなかった）。あわせて 2.5 の「公開でない記録は暗号にしてから出す」を、暗号の前に破らないため。pfs_repl は頭が "AKR" のブロックを announce も送りもせず、pfs_dag は `ark/` の名前を配らず受け取らない。したがって E1 の「region の 1 台の kill を越えて」は E3（暗号で複製）の後。目次の 1 ブロックの数は試験では 4（`PKERNEL_ARK_REC_FANOUT`、目次を作るときだけ読み、目次の頭に残す）。作る途中で芯のバグ PFS-ARK-HAS（台帳）を見つけて直した。限界: `ark/rec` の名前は refs.tab（`PKERNEL_PFS_DIR`）に残る＝ARK の像だけでは名前が戻らない。pfs_dag の manifest（名前と中身の id）は announce される。mw_gc を回す試験は無い（頭 4 バイトが MW_MAGIC でない、の読みだけ）。既存の `self/` の名前も 2 台で同じ形に食い合っていた＝A の同意で B の共感の門が開いた（台帳 SELF-REF-REGION-SHARED、直し fix/self-ref-local で `self/` もノードに留める）

## 5. E3（暗号で複製）の設計（2026-11-09 第 2 版。第 1 版は監査-169 FAIL＝目次がノードを出ず端末を失うと戻せない・AAD が外のブロックだけで組めない）

- **版ごとに新しい箱**（`arkc_box_new`）。封じる平文＝内の頭 32 B（rec_no・seq・kind・vis・learn・時刻・長さ）‖本文を **1 回だけ** 封じ（nonce 0）、3584 B ずつ断片 `AKSF` に。見せ方を変える版も本文ごと封じ直す（監査-158 の注）。平文の上限 440800 B
- **外の頭 `AKSR`**: magic・版・seq・前の `AKSR` の id・暗号文の長さ・断片の数・wrap 80・tag 16・断片の id（148 B＋32×n）。**AAD は tag より前の 132 B**＝外のブロックだけで開ける
- **鍵は本人に包む**: ノードの方舟の鍵（X25519、`PKERNEL_PFS_DIR/ark.key` 0600、無ければ作る。作れなければ何も書かない）。「継ぐ人に」の預けは v1.1
- **封じた目次も出す**: 今の目次（`AKR*`、ノードに残る）と同じ形の木を `AKSI`（平文＝数‖id、葉は各記録の最新の `AKSR`）と根を指す `AKSD`（nrec・段・fanout・根）で書く。どれも節ごとに新しい箱、AAD は頭の 96 B（magic・版・段・長さ・wrap）。書く順は AKSF→AKSR→AKRF→AKRR→葉..根（AKSI と AKRI）→AKSD→`ark/rec`。`ark rec ls` が `sealed=<AKSD の id>` と出す
- **（監査-170 ①）fanout は両方の木で同じ 124 以下**（AKSI の 1 ブロックに 124＝既定。前の既定 127 の目次は読めるが封じられない）。**（監査-170 ②）`vis=public` の版も封じて出す**（控え。公開の配り方は E6 の後）
- 頭が "AKS" のブロックは pfs_repl の送り止め（"AKR"）にかからず region へ複製される。**続けて書くと取りこぼす**（台帳 PFS-REPL-WANT-DROP、B は 36 個中 18 個）＝`ark rec sync` が 700 ms おきに出し直す（仲間 0 台なら再度。像だけのブロックも出す）。自動では呼ばない（限界）
- **戻し方** `ark rec restore <AKSD の id>`: 同じ `ark.key` を持ち記録の無いノードが WANT で取り、開いて AKR* と両方の目次を作り直す（AKSR・AKSF はそのまま使う）。`ark rec sget` は封じた写しから読む
- **限界**: 外から版の数・鎖・暗号文の長さ・目次の大きさは読める。戻すには `ark.key` の控えと AKSD の id が要る（最新を探す仕組みは次）。region が AKS* を持ち続けるかは pfs_repl しだい

### 合否（実装より先に固定、壊したら赤。`tests/host/run_ark_e3.sh`）
- E3a 2 台（中継）で A の 10240 B の記録 1 つと 3 版・印つきの記録・public にした記録: B の像に AKSR・AKSF・AKSI・AKSD があり、AKR* も本文の 16 B の並びも印も無い（A の像には印と AKRR がある＝走査の陽性の対照）
- E3k 3 版の鍵の指紋が 3 つとも違う。E3f 既定の fanout は 124
- E3b A が再起動して `sget` で全版を読める。断片の 1 ビットを変えると「開けない」と言い何も出さない
- E3c 違う `ark.key` のノードでは restore が開かない
- **E3d（E1 の region の kill）** B が A の最後の AKSD を持つのを見てから A を kill -9。A の `ark.key` を写した空の C が restore で全記録の全版を同じ sha256 で読め、`ls` が A の最後と同じ
- 陰性 `-DARK_REC_PLAIN`・`-DARK_REC_SAME_KEY`・`-DARK_REC_INDEX_LOCAL`・`-DARK_REC_AAD_INNER`（赤になる行は試験の頭に実測で書く）
- E3s（監査-170 の提案）AKSD の前で止めても、前の AKSD の id で前の版に戻る

## 6. E5（学習の印）の設計（2026-11-15、#43 の 2。試験を先に、実装は次の回）

master affd64cd を読んだ: 記録は `learn` を持つが、作るとき 0 で、変える口が無い。記録から学びへの道も無い（cradle の授業の輪 `cradle_lesson_ingest` に入るのは先生と網の授業だけ）＝今の E5 は「道が無いから入らない」だけで、何も確かめていない。
- **`ark rec learn <no> yes|no`**: 学習の同意だけを変えた新しい版（見せ方の `vis` と同じ形。本文は同じ）。同意の移り変わりは版の鎖に残る
- **`ark rec teach`**（持ち主がコンソールで。自動では呼ばない）: 各記録の最新の版のうち `learn=yes` だけを、番号の順に本文を `\n` でつないで 1 つの授業にし、`cradle_lesson_ingest` に渡す（良心の床もそこで通る）。輪（6144 B）に入りきらない記録は切らずに飛ばして数える。合わせて 128 B（`CRADLE_MIN_LIVE`）に満たなければ渡さない。出力 `[ark-rec] teach records=R skipped=S full=F bytes=B ring=<輪の sha256 の頭 16 桁>`。同意が 0 件なら `[ark-rec] teach nothing consented`（輪は前のまま）
- 読むのはノードの中の平文の版（AKR*）だけ。封じた写し（AKS*）から学ばない

### 合否（実装より先に固定、壊したら赤。`tests/host/run_ark_e5.sh`）
- **E5a** `fill` で 200 B の記録 2 つ（種 3・5）、0 だけ `learn 0 yes` → teach は records=1 skipped=1 bytes=200、ring が python で作った種 3 の本文の sha256 と同じ
- **E5b** `learn 1 yes` → records=2 bytes=401、ring＝種 3‖`\n`‖種 5。`learn 1 no` → また種 3 だけ。`log 1` は新しい順に learn=no・yes・no・no（最後の no は E5c のために書いた版）
- **E5c** 同意 0 件（どちらも no）→ nothing consented、`cradle probe` の ring_len が前と同じ
- **E5d** 再起動しても同意は最後のまま（teach が種 3 だけ）
- **E5e**（同じ回に足した。コードより先に ba271e68）同意を取り消すと（`learn 0 no`）、輪がまだ teach の授業のまま（sha256 が同じ）でその記録を含むなら輪を空にする: ring_len 200→0。学んでしまった重みは戻らない（限界、S1 の合体した重みと同じ）
- 陰性: `-DARK_REC_LEARN_ALL`（印を見ない＝E5a 赤）、`-DARK_REC_LEARN_NOVER`（`learn` が新しい版を書かず手元の値だけ変える＝E5d 赤）、`-DARK_REC_NO_WITHDRAW`（取り消しで輪を空にしない＝E5e 赤）
- 限界: 学んだかどうか（損失が下がるか）は見ない＝同意の門だけ。輪は 1 授業ずつ（次の teach や網の授業で置き換わる）。Android は良心の床が無いビルド（cradle.c の頭の注）

実装（2026-11-15 10:00 の回、feat/ark-e5、監査待ち）: ark_rec.c に `learn`（`pv_learn` を立てて put_version＝vis と同じ道で本文ごと封じ直す）と `teach`（`cmd_teach`）。ring の値は `cradle_window_src` で輪そのものを読んで sha256。測り（容器、x86_64）: 試験だけのコミット 3a4d2e18 では E5a〜E5d 全部赤、実装 29d3238a で全部緑、LEARN_ALL は E5a 赤・LEARN_NOVER は E5d 赤。E5e: 試験のコミット ba271e68 で E5e だけ赤、実装 50c0e099 で E5a〜E5e 全部緑、NO_WITHDRAW は E5e だけ赤、hosted の警告 57＝master と同じ。**E5c は弱い**: 輪は前も後も 0（空）＝「変わらない」は空のまま、を見ただけ（先に網の授業を入れてから同意 0 件の teach をする形にすれば強くなる）。ark_rec.c はベアと Windows のビルドに入らない

監査-181（2026-11-15 12:30 の回、実装していない回）: **限界つき PASS**。容器で先端 26086357 ALL PASS、3a4d2e18 は E5a〜E5d 赤、ba271e68 は E5e だけ赤、陰性 3 つとも狙いが赤（実装者の数字どおり）。私の壊し: 授業に入った番号を 1 ずらす → E5e 赤。**「授業に無い記録の取り消しでは輪を空にしない」の確かめを外す壊しは全段緑**（空にしすぎる向き＝安全側。素では輪 200 のまま、壊しで 0 を私の台本で見た）。sha256 の比べ（網の授業で置き換わった後）も試験の外。E5c の弱さは上のとおり。読んだ: teach は平文の版だけを読む、輪は置き換え（memcpy）で足し込まない＝sha256 の比べは筋が通る、Android は ark_rec.c が入り良心の床なしで授業が通る（上の限界）

## 7. 1 人でも使える入口（ark アプリの画面と口）の設計（2026-11-15、#43 の 2。docs だけ・未実装）
### 7.1 今あるもの（master affd64cd を読んだ）

- 記録は**シェルからだけ**（`ark rec new|set|vis|learn|teach`、`ark heir set <64 hex>`）。アプリの画面は galaxy（127.0.0.1 の HTTP、Android は WebView）で、口は `/profile`・`/teach`・`/ask` など。記録の口は無い
- galaxy の要求は 3072 B まで（`GX_REQ_MAX`）。同意の門 `ark_consent_ok`（README の原文への同意）は `/teach` にかかる
- 鍵 `ark.key` を失うと封じた写しは誰にも開けない（E3 の限界）。控えを取る画面は無い

### 7.2 決めたこと

1. **記録の口は galaxy に足す**（第 2 の server は作らない）: `POST /ark/rec`（新しい記録）・`POST /ark/rec/<no>`（新しい版）・`GET /ark/rec.json`（番号・版・時刻・見せ方・学習・長さだけ。**数・いいね・順位を出さない**＝E6）・`GET /ark/rec/<no>`（本文のバイト）・`POST /ark/rec/<no>/vis|learn`・`GET|POST /ark/heir`。中身はシェルと同じ関数（ark_rec.c）を呼ぶ
2. **書く口は同意の門の後ろ**（`ark_consent_ok`、`/teach` と同じ）。**読む口も書く口も、6 の合言葉が無ければ 401**（127.0.0.1 は「自分」の証にならない。監査-183）
3. **原文のまま**: 本文は受けたバイトをそのまま（文字コードの変換・空白の詰め・改行の直しをしない）。UTF-8 でなくても断らない。1 回の POST は 16 KB まで（この口だけ大きい受け皿）。長い記録は章ごとの記録に分ける（v1 の限界）
4. **最初の画面の順**: 原文（README）→ 同意 → **鍵の控え**（`POST /ark/key/backup`＝合言葉つきの POST で、`ark.key` を 64 桁の 16 進で 1 回だけ返す。2 回目からは 410。GET では出さない。「書き写した」の `POST /ark/key/backup/done` まで記録の口は 409。印は `ark.key.shown` に残し、再起動でも 2 回目は出さない）→ **継ぐ人**（相手の端末が見せる 64 桁を貼る、飛ばせる、身元は確かめない）→ 最初の記録。どれも後から変えられる
5. 見せ方の既定は「自分だけ」、学習の既定は「使わない」（ark-entry.md 2.4・2.6 のまま）。画面は 1 つずつ選ばせる（まとめて変えるボタンを置かない）
6. **呼び手の確かめ（監査-183 の ①②）**: galaxy は起動ごとに合言葉（32 B の乱数、64 桁の 16 進）を作り、`PKERNEL_PFS_DIR/galaxy.token`（0600、書いてから bind）に置く。要求は見出し `X-Galaxy-Token` で出す（比べは長さ固定・全バイト）。
   - 渡し方は 1 つ: **URL の `#t=<64 hex>`**（# の後ろは送られず Referer にも出ない）。Android は GalaxyActivity がアプリ専用の filesDir（他のアプリは読めない）から読んで `loadUrl` に付ける。PC はシェルの `galaxy url` が同じ形の URL を出す。ページの JS は sessionStorage に移し、`history.replaceState` で URL から消し、fetch の見出しに付ける
   - 独自の見出しはよそのページからは前置き（OPTIONS）なしに送れない。galaxy は OPTIONS を受けない＝ブラウザのよそのページは届かない。合言葉を持たない他のアプリは 401
   - **Host** が `127.0.0.1:<port>`・`localhost:<port>` でなければ 403（DNS の付け替え）。**Origin** があり `http://127.0.0.1:<port>`・`http://localhost:<port>` でなければ 403。Origin が無い（curl）は合言葉だけで見る
   - `/events`・`/ws` は見出しを付けられないので `?t=` で受ける（同じ比べ）
7. **鍵を戻す口（監査-183 の ④）**: シェルの `ark key import <64 hex>`。`ark.key` が無いときだけ書く（0600、pfs_dur_write）。あれば何もせず断る（ark_rec.c 269 行の「上書きしない」を守る）。HTTP の口は作らない（v1）

**既存の口（/teach・/profile・/ask・/profile.json・/console.txt・/log.txt）も同じ穴**（監査-183）。決めた: **galaxy の POST は全部と、人の中身を返す GET（/self.json＝human の欄がある・/profile.json・/console.txt・/log.txt・/events・/ws・/ark/*）は 6 の合言葉を要る**。要らないのはページの殻と公開の文だけ（`/`・/galaxy.json・/modules.json・/manifesto・/vocab・/langs）。**hosted だけ**（`_TK_HOSTED_LIBC_` の中。ベアの galaxy は ark の口も合言葉のファイルも無く、今のまま＝crown は動かない見込み。作る回が測る。ガードが hosted で本当に効くかは A7 が見る＝11-15 の B5W の「LLM_CFLAGS に定義が無い」の轍を踏まない）。他の案（/ark/* だけ守り、残りは台帳に行を足す）は採らない: 同じ確かめを 2 つの決まりで持つと片方が抜ける。直す呼び手（curl で POST する試験）: tests/host/run_self_ref_local.sh・run_stop_galaxy.sh・stop_galaxy.py・tests/web/consent_dom_cert.py・tests/android/emulator_smoke.sh・ci.yml の段。合言葉は試験も `galaxy.token` から読む

### 7.3 合否（作る前に固定。`tests/host/run_ark_app.sh`、galaxy に curl）

- **A1** 同意の前の `POST /ark/rec` は 403 で何も書かない。同意の後は `{no,seq}` で、`GET /ark/rec/<no>` が送ったバイトと sha256 で同じ（多バイトの文字・CRLF・行末の空白・NUL を含む本文で）
- **A2** HTTP で書いた記録が `ark rec ls` に出て、封じた目次の id（sealed=）が変わる
- **A3** `vis`・`learn` の POST が版を足し、既定は self・no
- **A4** 鍵の控えを「書き写した」にする前は記録の口が 409。`POST /ark/key/backup` の 2 回目（再起動の後も）は 410、GET は 405 か 404。控えの 64 桁を空のノードで `ark key import` → `ark rec restore` で本文が sha256 で同じ。`ark.key` があるノードへの import は断り、ファイルのバイトが変わらない
- **A5** `/ark/rec.json` と画面の HTML に数・順位の欄が無い（決めた語の一覧で走査）
- **A6** 16 KB を超える POST は 413 で何も書かない
- **A7** 7.2 の 6 の守る口（全部の POST と守る GET を表で回す）に、合言葉なし→401、違う合言葉（1 バイト違い・短い）→401、正しい合言葉で `Origin: http://evil.example`→403、`Host: evil.example:7800`→403、`OPTIONS`→405 か切断。どれも何も書かない（記録の数・profile の seq が前後で同じ）。公開の口は合言葉なしで 200。`galaxy.token` は 0600 で、起動ごとに違う
- **A8** ページの殻を `#t=` 付きで開いた試験（tests/web の DOM の試験）で、同意→記録が通る＝ページの JS が見出しを付けている
- 陰性: `-DGALAXY_ARK_NO_GATE`（A1 赤）・`-DGALAXY_ARK_NORMALIZE`（行末の空白を詰める＝A1 の sha256 赤）・`-DGALAXY_ARK_NO_BACKUP_GATE`（A4 赤）・`-DGALAXY_NO_TOKEN`（合言葉を見ない＝A7 赤）・`-DGALAXY_NO_ORIGIN`（Origin・Host を見ない＝A7 の 403 の 2 つだけ赤）・`-DGALAXY_BACKUP_TWICE`（控えを何度も返す＝A4 赤）

### 7.4 限界・まだ

- 鍵の控えは紙に書き写す形だけ（言葉の列や QR は次）。控えを見せる画面は 127.0.0.1 の上でも、画面を覗かれれば読める
- 継ぐ人の公開鍵の受け渡しは手で 64 桁（近くの 2 台で読み取る形は次）。渡す処理は v1.1（ark-handover.md）
- ark-profile.md の v3「会話の原文の保管庫」は、この記録（原本の層）で置き換える。会話を自動で記録にするのは脳が育ってから
- Android の実機で確かめるのは人の仕事
- 合言葉は同じ利用者の別のプロセス（PC で同じ人が動かすもの）からは読める。守るのは「よそのアプリ・よそのページ」まで。root・同じ uid は守らない
- 作る順: ① 合言葉と既存の口（A7・既存の試験の直し）を先に 1 本 ② 記録の口（A1〜A6・A8）を次の 1 本。①だけで今ある穴が閉じる

### 7.5 監査-183（2026-11-15、FAIL。設計の直しへ）

7.1 の事実はコードどおり（galaxy.c 712 行 GX_REQ_MAX、1495 行の門は /teach だけ、ark.key は X25519 の秘密 32 B）。**戻す理由: galaxy の口に「誰が呼んだか」の確かめが無い**（loopback に bind するだけ、Origin・合言葉なし）。Android では同じ端末のどのアプリも 127.0.0.1:7800〜 に繋げる、端末のブラウザのページは text/plain の POST を前置きなしで送れる。この口の上で 7.2 の 2（読むのは門なし）は「自分だけ」の記録を他のアプリに読ませ、4 は秘密鍵を見せ、書く口は同意の後なら他のページから書ける。直しのおすすめ: ① /ark/* は合言葉（起動ごとに作り 0600 のファイルか WebView にだけ渡す）を要り、無ければ 401 ② Origin・Host が自分でなければ 403 ③ 鍵の控えは合言葉つきの POST で 1 回だけ ④ A4 の「別のノードで restore」には鍵を戻す口（`ark key import`）が要るが 7.2 に無い ⑤ 合否に A7（合言葉なし 401・よその Origin 403）と陰性 `-DGALAXY_ARK_NO_TOKEN`
直し（11-15 23:30 の回）: ①②③⑤は 7.2 の 2・4・6 と 7.3 の A4・A7・A8・陰性 3 つに、④は 7.2 の 7 に入れた。既存の口の穴は 7.2 の 6 の後ろの段落で決めた（全部の POST と人の中身を返す GET に合言葉、台帳の行は足さない）。監査待ち（🔍ENT2）

### 7.6 監査-184（2026-11-16、限界つき PASS。直した回とは別の回。作る回への条件）

galaxy.c（master 2df75ce3）と突き合わせた。合っている: 口は gx_route の 14 本（`/`・galaxy.json・self.json・console.txt・modules.json・log.txt・manifesto・vocab・langs・ws・events・profile.json・POST の teach・ask・profile）で、7.2 の 6 の守る／公開の表に全部が入り、漏れも余りも無い。gx_parse は GET・POST 以外を -1 にし、呼び手（1676 行）は 400 を返して閉じる＝前置きは通らない。galaxy.c は hosted の 3 つ（linux_x86_64・linux の Makefile は CFLAGS＝KERNEL_CFLAGS に `-D_TK_HOSTED_LIBC_`、Android は add_compile_definitions、Windows も CFLAGS）で定義つき＝④の推論は正しい（読んだだけ、ビルドはしていない）。Android の loadUrl（GalaxyActivity.kt 275 行）は `?intro=1` を付けるだけ＝`#t=` を足す形は成り立つ。
条件（作る回で）:
1. **`?t=` が入らない**: GX_REQ の query は 64 B（`char query[64]`、63 文字で切る）。`t=`＋64 桁＝66 文字。/events・/ws の合言葉を受けるには query を広げるか、`t=` を別に読む。A7 に /events・/ws の正しい合言葉で 200（101）を足す
2. **直す呼び手の一覧が足りない**: samples/38_galaxy/galaxy_cert.sh（/teach・/ask・/profile・/events・/log.txt、ci.yml の Galaxy cert）・samples/39_ark_profile/profile_cert.sh・samples/40_i18n_manifesto/i18n_cert.sh・samples/41_shared_mind/run.sh・samples/lib/http_retry.sh（合わせて /teach・/profile・/profile.json・/self.json・/events・/ask。39 と 40 は ci.yml の段）も合言葉が要る（grep の範囲に samples/ が無かった）。tests/modver/run_modver.sh は /modules.json だけ＝公開のまま
3. A7 の OPTIONS は「405 か切断」でなく今の動き＝**400 で閉じる**も合格に数える
限界: /galaxy.json は公開のままだが、名乗った人の handle（star）を含む。handle は名乗ると決めた名前なので公開でよい、と読んだ（網に出ているかは確かめていない）。⑥の `ark key import` は壊れた ark.key を手で消す必要がある＝上書きしない側に倒した決めとして妥当。

### 7.7 作る順 ① を作った（2026-11-16 20:20 の回、feat/ark-token。監査待ち）

試験を先に（a8708f80・dcf6aedf、`tests/host/run_ark_app.sh`＋`ark_app.py`、A7 を 9 項目に分けた）。master（80eae153）では file・no-token・bad-token・origin・host・nowrite の 6 つが赤、public・good-token・options は緑（変えてはいけない側）。実装 0b54ece7（galaxy.c の gx_guard、hosted だけ＝galaxy.c はベアのビルドに入らない）:
- 合言葉は galaxy_init で 32 B（Linux・Android は sign_entropy＝/dev/urandom、Windows は rand_s。弱い乱数なら作らず、守る口は 503）。PKERNEL_PFS_DIR があれば `galaxy.token`（pfs_dur_write、0600）、無ければ自分のコンソールに `[galaxy] open http://127.0.0.1:<port>/#t=…` を 1 行
- 守る口は「公開の 6 本（`/`・galaxy.json・modules.json・manifesto・vocab・langs）以外の全部」＝知らない道も 401（既定で閉じる）。§7.6 の 1 は `t=` を生の要求行から読む（GX_REQ.query は広げない）、`?t=` は /events・/ws だけ
- Host は全部の口で `127.0.0.1:<port>`・`localhost:<port>` だけ（無い Host も 403）。Origin は有れば `http://` 付きの同じ 2 つだけ（公開の口も）
- ページの JS は `#t=` を sessionStorage へ移して URL から消し、相対の fetch 全部に見出しを付ける（window.fetch を包む）。Android は filesDir/ark/galaxy.token を読んで `#t=` を付ける
呼び手（§7.6 の 2）: samples/38〜41・lib/http_retry.sh（`GX_TOKEN_FILE`）・run_self_ref_local・stop_galaxy.py・emulator_smoke（`run-as`）。ci.yml の ump-x86_64 に段。
限界: ブラウザが自分で取りに来る /favicon.ico も 401 になる（前は 404）。Android の実機と WebView は試していない（人の仕事）。ページの JS は使い捨ての確かめ（スタブのサーバー＋headless chromium、枝 20 本中 19 本に合言葉・master は 19 本とも無し）だけで、A8 は ② と一緒
測り（容器 /build/tok）: 8a547355 で plain 9/9・nc-token は no-token・bad-token・nowrite だけ赤・nc-origin は origin・host だけ赤（ALL PASS）。0b54ece7 の最初の走りは nowrite が赤＝試験側が合言葉つきで sea を教えていた（8a547355 で ink に）。crown は master と全バイト同じ（x86 .text cd490552・aarch64 4dfac3f4、.rodata も）、hosted x86_64 の警告 57 で master と同じ

### 7.8 監査-185（2026-11-18、限界つき PASS。作った回とは別の回）

取り直し（容器 /build/a185）: 先端 06e7bc99 で ALL PASS（3 腕とも 7.7 と同じ行）、試験だけの dcf6aedf で file・no-token・bad-token・origin・host・nowrite の 6 赤。crown は master 80eae153 と全バイト同じ、hosted x86_64 警告 57・aarch64 OK。呼び手 6 本（self_ref・38・39・40・41・stop_galaxy）とも緑。私の壊し: Host を見ない→host 赤、/self.json を公開に→no-token・bad-token 赤、`?t=` を全部の GET で→bad-token 赤、**比べで先頭の 1 字を飛ばす→9 項目とも緑＝H30**。読み: gx_header_val は本文まで探すが最初の一致＝本物の見出しが勝つ。
限界: ページの JS は `u.charAt(0)==="/"` で `//よその host/` にも見出しを付ける（今の fetch は全部 `/英字` の固定の道＝害なし、② で `//` を除く）。emulator_smoke は 401 の本文も「答えた」と数える（合言葉が読めないと後段で紛らわしい赤）。ほかは 7.7 の限界のまま。

### 7.9 作る順 ② を作った（2026-11-19 00:40 の回、feat/ark-rec-http。監査-189 で限界つき PASS＝7.10）

試験先 5d2f5b8c（A1-gate・A1・A2・A3・A4・A4-import・A5・A6 と陰性 4 腕、`run_ark_app.sh`）は master で part ② が全部赤。実装（galaxy.c の gx_ark_route、ark_rec.c の ark_rec_http_*・ark_key_backup_*・`ark key import`、hosted で Windows は除く）: 道の順は 413（16 KB 超）→ 403（同意の前の書き込み）→ 409（鍵の控えを書き写す前、読む道も）。/ark/rec の POST は 16 KB の大きな受け皿 1 つで受ける（使用中は 503）。印は ark.key.shown（鍵を渡す前に書く）と ark.key.done。import は印も立てる。ページの JS は `//`・`/\` の道に合言葉を付けない（7.8 の限界）。GET|POST /ark/heir（AH、試験先 935f9817 で赤→8a433c06）も同じ門の後ろ。A8（DOM の試験）と画面（同意→控え→継ぐ人→最初の記録）はまだ＝容器に Playwright が無い。
限界: シェルと HTTP は ark_rec.c の同じ置き場を使い、旗で 503 を返すが旗は不可分でない。A4-import はブロックのファイルを写して網の代わりにした。A5 の最初の走りは私の語の一覧の誤り（WebViews）で赤、語の境で比べる形に直した。

### 7.10 監査-189（2026-11-19、限界つき PASS。作った回とは別の回）

先端 96483068 で 7 腕を取り直し ALL PASS（plain 18/18、陰性は各々狙いが赤）。試験先 5d2f5b8c で part ② 8 つ赤。私の壊し: import が在る ark.key を上書き→A4-import だけ赤、413 を外す→A6 だけ赤。galaxy の呼び手 39・40・41 も先端で PASS（実装者の 38・self_ref_local・stop_galaxy と合わせて全部）。限界: ① **7.9 の「シェルと HTTP は旗で 503」は誤り**。旗 http_busy を立てるのは ark_rec_http_* だけで、シェル（ark_rec_cmd）は立てない＝シェルと galaxy のタスクが同じ body・rec・desc を同時に使いうる（hosted はタイマーで横取りする）。実測ではシェルの `ark rec fill` と POST /ark/rec を交互に 2×40 回、80 件とも番号が別・読み戻し一致で壊れは出なかった。直し: ark_rec_cmd も同じ旗を取る（作る回で） ② A1-gate は控えの前（409 でも止まる状態）でしか試していない。nc-ark-gate の赤は 403→409 の違いで、「控え済み・同意前」の書き込みは試験に無い ③ 大きな受け皿は合言葉の確かめ（本文の後）より前に取られる。本文を送らずに止まる相手は受け皿を持ち続ける（galaxy には idle の打ち切りが無い、どの口も同じ） ④ gx_send_raw は相手が読まないと回り続ける（合言葉の後）

### 7.11 7.10 の ① ② を直した（2026-11-19 05:10 の回、feat/ark-rec-lock。監査待ち）

① シェルの `ark rec|heir|key` も同じ旗を取る（ark_rec_cmd、不可分の交換 `__atomic_exchange_n`）。中に相手がいればページは 503、シェルは `[ark] busy: ...` の 1 行で何もしない。試験 A9-lock（`run_ark_app.sh` の腕 lock、`-DARK_REC_TEST_HOLD` で入った側が PKERNEL_ARK_REC_HOLD_MS＝3 秒だけ旗を持つ）: D1 シェルが中→POST は待たずに 503・記録は 1 件、D2 ページが中→`ark rec ls` は busy・POST は 200・2 件。陰性 `-DARK_REC_SHELL_NO_FLAG` で赤。② A1-gate2: `ark key import` で控えの印がある・同意の無いノードへの POST は 403・0 件（nc-ark-gate で赤）。限界: 鍵の控えの道（ark_key_backup_hex）は旗を取らない（ark_priv を読むだけ）。待ちの口は試験のビルドだけ＝ふだんの二進で取り合いを起こす試験は無い
