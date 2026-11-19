# N2c: ノードの同一性（鍵を残す・番号の引き直し・incarnation）設計

2026-11-15 10:00 の回（docs だけ・未実装）。n2-wire16.md の決めたこと 5・7 の続き。

## 1. いまの姿（master c02cae26 を読んだ）

- **鍵は起動ごとに作り直し**: sign.c `sign_node_key_ensure` は毎回 `sign_entropy` から種を作る＝再起動すると別人の署名になる
- **番号の種** node_id.c: `$PKERNEL_PFS_DIR/node_seed`（16 B を hex）。書くのは O_TRUNC で fsync なし、短い・壊れたファイルは**黙って作り直す**＝番号が無言で変わる。番号は 1..63
- **incarnation** swim.c `my_incarnation` は 1 バイト・保存しない＝再起動で 0。前の命の数の DEAD のうわさが ALIVE(0) に勝つ（本人が反論するまで）。v2 も 1 バイト
- **許可の一覧** sign.c はメモリの中だけ・既定は空。使うのは入れるコードの署名（selfc・genome・OTA）だけ

## 2. 決めたこと（他の案は §4）

1. **鍵を残す**: hosted は `node.key`（Ed25519 の種 32 B）を node_seed の横に 0600 で。書くのは一時ファイル→fsync→rename。読めない・長さ違いのときは**上書きしない**: `node.key.bad` に退けてから作り、1 行言う。node_seed も同じ書き方に直す。ベアは今のまま（crown）
2. **番号の既定は狭いまま**（node_seed から 1..63）。公開鍵からの広い番号（`hash16(公開鍵 ‖ 引き直しの回数)`、257..65534）は `PKERNEL_NODE_WIDE=1` のときだけ＝旧版から見えなくなるノードを黙って増やさない
3. **ぶつかったら**: v2 のビーコンの後ろに公開鍵の指紋 8 B。同じ番号で指紋が違えば、指紋の大きい方が引き直す。回数は `node.redraw` に fsync してから使う。狭い番号のぶつかりは言うだけ（引き直すと旧版との約束が変わる）
4. **incarnation を残す**: 起動で `node.incarn`＋1 を fsync してから最初のビーコン。電文は 1 バイトのまま（下の 8 ビット）。2 バイトにするのは v2 の版を上げる別の切れ（256 回の再起動で 1 回の取りこぼし＝今は急がない）
5. **同じ番号は同じ鍵**（初めて見た鍵を覚える）: 知っている番号の指紋が変わったら別人として扱い、前の状態（seq・持ち手）を引き継がない。1 行言う。許可の一覧で仲間を選ぶのは #9 の 1（ここでは作らない）

## 3. 合否と陰性（実装の前に固定）

- **I1** 2 回の再起動で公開鍵と番号が同じ（狭い・広いの両方）。陰性 `-DN2C_NC_KEY_EPHEMERAL`（毎回作る）で赤
- **I2** node.key を 5 B に切ると、起動して node.key.bad が残り、1 行言う。陰性 `-DN2C_NC_KEY_OVERWRITE` で赤
- **I3** 広い 2 台を同じ番号・別の鍵で起こすと、30 秒以内に 1 台だけ番号が変わり、教え（X3 の形）を取り違えない。陰性 `-DN2C_NC_NO_REDRAW` で赤
- **I4** 引き直した 1 台を再起動すると同じ引き直しの番号。陰性 `-DN2C_NC_REDRAW_VOLATILE` で赤
- **I5** kill -9 で DEAD にされたノードを再起動すると、仲間が 1 周（ビーコンの間隔）以内に ALIVE。陰性 `-DN2C_NC_INC_RESET`。**この陰性が赤になるかは測ってから言う**（反論で直るなら効きは小さい）
- **I6** 指紋の変わった番号を別人として扱う（`nodes idpin`＋生きた 1 本）
- 共通: [n2-mix] X1・X2・X5 が緑のまま、ベアの .text・.rodata が全バイト同じ

## 4. 他の案と限界

- 番号の既定を公開鍵から（決めたこと 5 の元の形）: 新しいノードが全部旧版から消える。N3 で
- incarnation を電文で 2 バイトに: 版の交渉が要る。保存で足りると読んだ（I5 で確かめる）
- 限界: 初めて見た鍵を信じる（TOFU）。鍵を失えば別人。Sybil は防がない（#9 の 4）。切れの順は 1→4→2・3→5

## 5. 監査-183（2026-11-15、限界つき PASS。作る回への条件）

§1 は 4 つともコードどおり（sign.c 55 行、node_id.c 109〜140 行、swim.c 131 行の UB、allow_pk はメモリ）。条件: ① **狭い番号のぶつかりで 5 が揺れる**（同じ番号・別の鍵の 2 台が交互に聞こえると、そのたび別人として状態を捨てる）＝狭いぶつかりは最初の鍵を保ち、もう一方は言うだけ。I6 に「狭い 2 台が同じ番号で状態の捨てが 1 回以下」 ② 指紋は v2 のビーコンにだけ載る＝v1 だけの狭いノードのぶつかりは見えない、と書く ③ 鍵が残ると genome.c 548・gen_succession.c 368・lm_self.c 606 の「自分の鍵」が再起動をまたぐ＝実装の回はこの 3 つを読む ④ 小さい指紋を作れる者は相手を引き直させ続けられる（Sybil と同じ限界）。`leave` は node.key を残す（node_seed と同じ）

## 6. 第 1 段（2026-11-19、feat/n2c-identity、監査待ち）

決めたこと 1・4 だけ。arch/linux/node_ident.c（hosted、sign.c・swim.c は `_TK_HOSTED_LIBC_` の中だけ＝ベア不変）。試験 tests/host/run_n2c_identity.sh（I1・I1e・I2・I2b・I2s・IN、陰性 4）。I2 に足した形: node.key.bad も在れば何も上書きせず、その回だけの鍵で動く。弱い乱数の鍵は残さない。語 `sign key`。限界: 生きている間の反論で上がった incarnation は残さない（起動の +1 だけ）。Windows は rename が上書きしない＝2 回目から node.incarn が書けない（1 行言う。読み、測っていない。pfs_dur_write 全体の話）。I5 は §7

## 7. I5 の測り（2026-11-19、門ではない）

relay＋3 台、node 2 を kill -9、DEAD の後に同じ dir で起こす（台本 pk-scratch/n2c/i5.sh、2 腕同時に 1 回ずつ）。戻るまで: 実装 1.1 s・0.0 s、陰性 INC_RESET 0.7 s・0.3 s＝**陰性は赤にならない**。直に届く電文は incarnation を見ずに ALIVE に戻す（swim_rx の「recovered (via rx)」）ので、中継で全員が直に聞こえる網では保存の効きは見えない。効くのは噂だけで聞く仲間のとき（読み）。I5 は門にしない

## 8. 監査-189（2026-11-19、限界つき PASS）

先端 5741cca1 を容器で取り直した: crown x86・aarch64 の .text・.rodata が master と全バイト同じ、hosted x86_64 警告 57（master と同じ）・aarch64・Windows OK。試験 ALL PASS、陰性 4 つとも狙いが赤（KEY_EPHEMERAL は I2・I2b も赤）。試験先 b4e73c63 で I1・I2・I2b・I2s・IN 赤、I1e 緑＝実装者と同じ。私の壊し: move_aside が .bad を上書き→I2b だけ赤。**node_seed.bad も在るときに上書き→全部緑＝穴 H31**。網あり（relay なしの port）でも 2 回の起動で鍵・番号が同じ・incarnation 1→2。node.incarn を 2 B に切る→.bad へ退けて 1 から、255→256（電文は 0、§2 4 のとおり）。§5 の ③ は読んだ: 3 つとも呼んだときの鍵を pin する自己試験＝鍵が残っても壊れない。限界: **末尾に改行のある node_seed（33 B）は master では受け入れたが、今は .bad に退けて番号が変わる**（1 行言う。リポジトリに改行つきで書く所は無い。手で書いた人だけ）。同じ PKERNEL_PFS_DIR を 2 台で使えば同じ鍵（node_seed と同じ、前からの形）。Windows の rename は測っていない

## 9. 8 の限界と H31 を直した（2026-11-19 05:10 の回、feat/ark-rec-lock。監査待ち）

node_seed の 32 hex の後の LF・CRLF は同じ種として読む（master と同じ扱い、ほかの尾は今までどおり .bad へ）。試験 I2n（LF・CRLF で同じ番号・行なし・.bad なし、b60f0d6c の前では赤）。H31: I2sb（.bad が在るときに切った node_seed を置くと、どちらも書き換えず「there too」の 1 行）、陰性 `-DN2C_NC_SEED_BAD_OVERWRITE` で赤

監査-193（2026-11-19 19:05 の回、作った回とは別）: PASS。run_n2c_identity.sh 6 モード ALL PASS を再現（I2n は id が LF・CRLF でも同じ・行 0・.bad なし、陰性 SEED_BAD_OVERWRITE は I2sb だけ赤＝.bad が vwxyz に上書き）。読み: 尾が「LF 2 つ」「CR だけ」「34 B より長い」は今までどおり .bad（read は 34 B まで）。H31 は「ふさいだ」（S22）

## 10. 第 2 段の設計: incarnation で生死を決める（2026-11-20 04:55 の回、docs と測りだけ）

**題**（§7・swim.c を読んだ）: DEAD にした仲間を、どの電文でも（直の rx・直／間接の探りの ACK）その場で ALIVE に戻し、**incarnation は DEAD のときの k のまま** ALIVE@k を噂する。ほかのノードは同じ k なら DEAD が勝つので捨てる＝見え方が割れたまま、直に聞くか探るまで直らない。だから保存した incarnation の効きも I5 で見えなかった。もう 1 つ: 比べが `inc_in > inc_cur` の素の大小＝255 の後の 0 は「古い」。噂 DEAD@255 への反論は 0 になり負け続ける（今は rx の復活が隠している）

**決めたこと**（hosted だけ、`_TK_HOSTED_LIBC_` の中。ベアは今のまま＝crown 不変）:
1. **DEAD からは、本人の言葉（うわさの自分の行）で k より新しい incarnation が来たときだけ ALIVE に戻す**。rx・探りの ACK では戻さない。SUSPECT は今のまま（直の証拠で ALIVE に戻す）
2. **知らせる**: DEAD の仲間から新しくない電文が来たら DEAD@k を gossip_add（TTL を新しく）。次のビーコンで本人に届き、本人が k+1 で反論する（6 月からある道、旧版もベアも反論する）
3. **比べは順序数**（RFC 1982）: `(UB)(a−b)` が 1..127 なら新しい。反論・ほかのノードの比べも同じ関数
4. **反論で上げた incarnation も node.incarn に書く**（§6 の限界。書けなくても 2 の道で戻れる）
5. 戻した行は `[swim] node N back at incarnation a (dead at k)` と 1 行言う

**合否（実装の前に固定、§3 に続けて）**:
- **I7**（unit `nodes life`、`[swim-life] PASS`）: a DEAD@5 の仲間から自分の行なし／ALIVE@5 の電文→DEAD のまま・DEAD@5 が積まれる b ALIVE@6→ALIVE・incarn 6 c 探りの ACK でも DEAD のまま d DEAD@255 に ALIVE@0→ALIVE e ALIVE@0 に DEAD@255→捨てる f 噂 DEAD@255 への自分の反論が 0 で、その 0 が別の受け手の DEAD@255 に勝つ g SUSPECT@5 は直の rx で ALIVE（今のまま）
- **I8**（生きた、relay＋3 台）: node 2 を kill -STOP で DEAD にし、TTL（6 周）を過ぎてから CONT。n1・n3 の戻りは「back at incarnation」の行で、rx・探りの戻り行は無い。node 2 は SELF-SUSPICION 1 行以上・incarnation が上がる。60 秒以内
- **I5b**: §7 の kill -9 と再起動を、保存あり・`N2C_NC_INC_RESET` の両方で。どちらも 60 秒以内に戻る（RESET は 2 の道で）
- 陰性: `-DN2C_NC_RX_REVIVE`（rx で戻す）I7 a・c と I8 が赤／`-DN2C_NC_NO_TELL` I7 a と I8 が赤（戻れない）／`-DN2C_NC_PLAIN_CMP` I7 d・e・f が赤
- 共通: `nodes test`（swim-incarn）・`cap`・`tea`・`v2`・[n2-mix] X1・X2・X5・[n2-alive] が緑、ベアの .text・.rodata 全バイト同じ

**限界**: 戻りが最短 1 周遅れる（2 の往復）。広い仲間（v2 のうわさは incarnation 0、生死は古さだけ）は入れない。旧版の hosted は素の大小のまま＝255 の近くでは割れうる。差が 128 以上は比べられない。新しい命の仲間の状態（seq・持ち手）を捨てるのは第 3 段（§2 の 5 と一緒に）
