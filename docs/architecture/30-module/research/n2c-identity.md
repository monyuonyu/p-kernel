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

決めたこと 1・4 だけ。arch/linux/node_ident.c（hosted、sign.c・swim.c は `_TK_HOSTED_LIBC_` の中だけ＝ベア不変）。試験 tests/host/run_n2c_identity.sh（I1・I1e・I2・I2b・I2s・IN、陰性 4）。I2 に足した形: node.key.bad も在れば何も上書きせず、その回だけの鍵で動く。弱い乱数の鍵は残さない。語 `sign key`。限界: 生きている間の反論で上がった incarnation は残さない（起動の +1 だけ）。Windows は rename が上書きしない＝2 回目から node.incarn が書けない（1 行言う。読み、測っていない。pfs_dur_write 全体の話。**→ 2026-11-21 fix/win-dur-rename で MoveFileExA に、試験は CI の windows-durable、監査待ち**）。I5 は §7

## 7. I5 の測り（2026-11-19、門ではない）

relay＋3 台、node 2 を kill -9、DEAD の後に同じ dir で起こす（台本 pk-scratch/n2c/i5.sh、2 腕同時に 1 回ずつ）。戻るまで: 実装 1.1 s・0.0 s、陰性 INC_RESET 0.7 s・0.3 s＝**陰性は赤にならない**。直に届く電文は incarnation を見ずに ALIVE に戻す（swim_rx の「recovered (via rx)」）ので、中継で全員が直に聞こえる網では保存の効きは見えない。効くのは噂だけで聞く仲間のとき（読み）。I5 は門にしない

## 8. 監査-189（2026-11-19、限界つき PASS）

先端 5741cca1 を容器で取り直した: crown x86・aarch64 の .text・.rodata が master と全バイト同じ、hosted x86_64 警告 57（master と同じ）・aarch64・Windows OK。試験 ALL PASS、陰性 4 つとも狙いが赤（KEY_EPHEMERAL は I2・I2b も赤）。試験先 b4e73c63 で I1・I2・I2b・I2s・IN 赤、I1e 緑＝実装者と同じ。私の壊し: move_aside が .bad を上書き→I2b だけ赤。**node_seed.bad も在るときに上書き→全部緑＝穴 H31**。網あり（relay なしの port）でも 2 回の起動で鍵・番号が同じ・incarnation 1→2。node.incarn を 2 B に切る→.bad へ退けて 1 から、255→256（電文は 0、§2 4 のとおり）。§5 の ③ は読んだ: 3 つとも呼んだときの鍵を pin する自己試験＝鍵が残っても壊れない。限界: **末尾に改行のある node_seed（33 B）は master では受け入れたが、今は .bad に退けて番号が変わる**（1 行言う。リポジトリに改行つきで書く所は無い。手で書いた人だけ）。同じ PKERNEL_PFS_DIR を 2 台で使えば同じ鍵（node_seed と同じ、前からの形）。Windows の rename は測っていない

## 9. 8 の限界と H31 を直した（2026-11-19 05:10 の回、feat/ark-rec-lock。監査待ち）

node_seed の 32 hex の後の LF・CRLF は同じ種として読む（master と同じ扱い、ほかの尾は今までどおり .bad へ）。試験 I2n（LF・CRLF で同じ番号・行なし・.bad なし、b60f0d6c の前では赤）。H31: I2sb（.bad が在るときに切った node_seed を置くと、どちらも書き換えず「there too」の 1 行）、陰性 `-DN2C_NC_SEED_BAD_OVERWRITE` で赤

監査-193（2026-11-19 19:05 の回、作った回とは別）: PASS。run_n2c_identity.sh 6 モード ALL PASS を再現（I2n は id が LF・CRLF でも同じ・行 0・.bad なし、陰性 SEED_BAD_OVERWRITE は I2sb だけ赤＝.bad が vwxyz に上書き）。読み: 尾が「LF 2 つ」「CR だけ」「34 B より長い」は今までどおり .bad（read は 34 B まで）。H31 は「ふさいだ」（S22）

## 10. 第 2 段の設計: incarnation で生死を決める（2026-11-20 04:55 の回、docs と測りだけ）

**題**（swim.c・drpc.c を読んだ）: DEAD の仲間を rx・探りの ACK・drpc の HEARTBEAT で ALIVE に戻し、死んだときの k のまま ALIVE@k を噂する。他は同じ k なら DEAD を残す＝見え方が割れる（§7 で効きが見えなかった理由）。比べも素の大小で、DEAD@255 への反論 0 は負ける

**決めたこと**（hosted だけ＝crown 不変）:
1. DEAD からは**本人の言葉（うわさの自分の行）で k より新しい incarnation**のときだけ ALIVE に。rx・探りの ACK・drpc.c 535 行の HEARTBEAT（ベアが送る）では戻さない。SUSPECT は今のまま
2. **知らせる**: DEAD の仲間から新しくない電文が来たら DEAD@k を gossip_add。本人が k+1 で反論する（旧版・ベアも反論する）
3. 比べは**順序数**（RFC 1982）: `(UB)(a−b)` が 1..127 なら新しい
4. 反論で上げた incarnation も node.incarn に書く（§6 の限界）
5. 戻した行 `[swim] node N back at incarnation a (dead at k)`。後始末は rx の戻りと同じ（replica_push_to・degrade_update）

**合否（実装の前に固定）**:
- **I7**（unit `nodes life`）: a DEAD@5 に自分の行なし／ALIVE@5 の rx→DEAD のまま・DEAD@5 が積まれる b ALIVE@6→ALIVE・6 c 探りの ACK・HEARTBEAT でも DEAD のまま d DEAD@255 に噂 ALIVE@0→ALIVE e ALIVE@0 に噂 DEAD@255→捨てる f 噂 DEAD@255 への反論 0 が DEAD@255 に勝つ g SUSPECT@5 は rx で ALIVE
- **I8**（relay＋3 台）: node 2 を kill -STOP で DEAD、20 秒（TTL の後）待って CONT。n1・n3 の戻りは「back at」の行だけ（rx・探りの戻り行 0）、60 秒以内
- **I5b**: kill -9、DEAD から 20 秒待って同じ dir で再起動。保存あり・`N2C_NC_INC_RESET` とも 60 秒以内に戻る
- 陰性: `N2C_NC_RX_REVIVE` I7 a・c と I8 が赤／`N2C_NC_NO_TELL` I7 a と I5b の RESET が赤／`N2C_NC_PLAIN_CMP` I7 d・e・f が赤
- 共通: `nodes test`・`cap`・`tea`・`v2`・[n2-mix] X1・X2・X5・[n2-alive] 緑、ベアの .text・.rodata 全バイト同じ

**master で測った**（試験先、pk-scratch/n2c2/）: I7 は a1・a2・d・e・f の勝ちが赤、b・g 緑（c は読みだけ）。I8 は 2/2 赤（n1・n3 とも「recovered (via rx)」）。**CONT の後 node 2 は 14・16 回反論した**＝止まる間に受けの口に溜まった古い噂を読む。NO_TELL は I8 で赤にならない見込み→陰性を I5b の RESET へ（測った後の直し、合否の線は同じ）。「SELF-SUSPICION がある」も master で緑なので合否から外した。I5b は master で両腕とも 1 秒未満で戻り（n3 は保存ありで gossip の ALIVE、RESET で rx）、**RESET の node 2 は 30 秒 SELF-SUSPICION 0**＝TTL の後は噂が届かない＝NO_TELL はそこで効く見込み

**参照**（memberlist state.go の原文）: aliveNode は既知の仲間（Dead も）に incarnation ≤ の alive を捨てる。refute は告発より上へ。probe は Dead を飛ばす。32 ビットなので順序数は無い

**限界**: 戻りが最短 1 周遅れる。広い仲間（v2 の噂は incarnation 0、生死は古さだけ）は入れない。旧版の hosted は素の大小＝255 の近くで割れうる。差 128 以上は比べられない。新しい命の仲間の状態（seq・持ち手）を捨てるのは第 3 段（§2 の 5 と）

**第 2 段の実装（11-20 19:05 の回、feat/n2c-stage2、監査待ち）**: 試験先 4ebbe8c7（`nodes life`＝I7 a〜g、`tests/host/run_n2c_life.sh`＝I7・I8・I5b と陰性 3 つ）→ 実装 feb75622（§10 の 1〜5、hosted だけ。陰性 `N2C_NC_RX_REVIVE`・`NO_TELL`・`PLAIN_CMP`）。探りの ACK の後始末は hosted で `swim_probe_ok` に切り出した（I7 c）。ベアは swim_task の 2 か所を書き下したまま（always_inline の関数にしても aarch64 の .text が動いた＝4ebbe8c7 で測った）。数字はコミットとバトン。**限界（条件 ③）**: DEAD の相手から電文が来るたびに DEAD@k を積み直す（TTL が新しくなる）＝反論しない相手（旧版の hosted は反論するが、素の大小で比べる）には噂が止まらない。電文は増えない（相乗り）。反論の incarnation を node.incarn に書く（自己試験の分は下の監査-200 で止めた）

**監査-197（11-20、限界つき PASS）**: 合否は測りより前（9e58ff51 04:58:57、探りの初出 04:59:42）。測った後の直しは線を弱めない（I8 の node 2 は止まる間に溜まった古い SUSPECT に反論して戻る＝NO_TELL は I8 では見えない。「back at」の行は a>k を含む）。DEAD の噂は移るとき 1 度だけ積まれ、TTL は送るごとに減る＝20 秒で足りる。HEARTBEAT はベアが混ざるときだけ届く。master 77f47bb9 で取り直し: I7 同じ赤、I8 2/2 赤（rx）、I5b 両腕 1.1 秒・RESET の SELF-SUSPICION 0。作る回への条件: ① I7 c を試験に（探りの ACK を関数に、HEARTBEAT は drpc_rx に電文を渡す）② i5b.sh の戻りの数え方に「back at」を足す ③ 知らせは電文ごとに TTL を新しくする＝反論しない相手には止まらない（限界に書く）

**監査-200（11-20、FAIL）→ 直し（11-20 22:15 の回、監査待ち）**: 自己試験が my_incarnation を 0 や 200 に置いて反論の道を通り、その値が node.incarn に書かれていた（`nodes test` 3→258、`nodes life` 3→256。次の起動の線の値が相手の知る値より古くなりうる＝§10 の 4 が 1 回崩れる）。swim.c の自己試験 9 本の間は数え `sw_selftest_depth` を上げ、反論は 0 のときだけ書く（hosted だけ、ベアは空のマクロ）。試験 I9（`nodes test|life|cap|teacher|v2cap|wide|selfelect` の前後で 3 のまま）、陰性 `N2C_NC_NOTE_IN_TEST`。限界: 自己試験の最中に本物の反論が来ても書かない（自己試験は人が打つときだけ）。`SWIM_SELFSUSPECT_CERT`・`NOFLEETSPLIT_CERT` のビルドの 2 本は数えを入れたが I9 では打っていない

**監査-201（11-21、限界つき PASS）**: 取り直しで試験先 I9 赤・直し ALL PASS（全 6 モード）・crown 全バイト同じ。壊し: life の数えを外す＝I9 赤、incarn 試験の LEAVE 忘れ・反論を一切書かない＝どちらも緑（§10 の 4 に試験が無かった）→ 監査側で I8 に足した: node 2 が STOP の前に `nodes test`、CONT の後 node.incarn ≥ 戻った incarnation、陰性 `N2C_NC_NOTE_NEVER`（test-holes S24）。限界: `nodes test` を打った I8 では戻りの incarnation が 7〜17 と揺れた（打たない full では 5・7）。原因は見ていない（害は数が進むだけ）。この項目は 1 回ずつしか走らせていない

## 11. 第 3 段（2026-11-21 05:25 の回、feat/n2c-stage3、監査待ち）

§2 の 2・3・5 を作った（hosted だけ）。**読んで分かった前提の違い**: 中継は番号ごとに席を 1 つ持ち、放送は送り手の席に返さない＝同じ番号の 2 台は互いのビーコンを直接聞けない。だから知らせは第三者の噂に乗せる:
- 番号: `PKERNEL_NODE_WIDE=1` かつ `PKERNEL_NODE_ID` なしのとき `257 + sha256(公開鍵 ‖ 回数 LE4) の頭 2 B mod 65278`（node_ident.c `pk_node_wide_id`）。回数は `node.redraw`
- v2 ビーコンの 29 B の後ろに `"N2F" n 指紋×n`（[0] 送り手、[1+i] 噂 i の番号に覚えている鍵）。旧版は 29 B だけ読む
- 鍵を覚えるのは本人のビーコンからだけ。違う鍵が来たら別人＝往復の測りを捨て、新しい鍵を覚え、1 行（§2 5）。覚え直すので、ぶつかっている間は第三者の噂が 2 つの鍵を交互に運ぶ＝両方が気づき、**鍵の大きい方が引き直す**（§2 3 のとおり）。最初の鍵を保つ形（監査-183 ①）にすると、第三者が皆 A を覚えたとき大きい B が永久に気づかない
- **作ってから分かったこと**: 中継と受け手は送り手番号ごとに再送よけの窓を 1 つ持つ。nonce を起動の秒から数えると、後に起きた方だけが通り、もう一方は聞かれも聞きもしない（f5258a75 で I3 赤、C の鍵の入れ替わりは 1 回だけ）。直し: 引いた広い番号のノードは nonce を壁時計（秒<<24 と同じ物差し）から取り、前の値+1 を下回らない＝両方通る（陰性 `N2C_NC_NONCE_COUNT`）。到着が約 4 µs より入れ替わると落ちるのはぶつかりの間だけ
- **もう 1 つ**（1b884f01、起動を 2 秒ずらした形）: 中継の席も第三者の噂も「最後に話した方」に向くので、大きい側に小さい鍵の噂が届かず、C が 80 回入れ替えても 30 秒誰も引き直さなかった。直し: 小さい側は、初めて相手の鍵を聞いてから 10 秒後にまだ聞こえたら自分が引き直す（大きい側が動けば聞こえなくなる）。陰性 `N2C_NC_SMALL_NEVER`（I6u）
- 引き直し: 回数+1 を fsync してから自分を execv し直す（番号は送り口・drpc・番地に起動時に固まる）。Android・Windows は 1 行言うだけ（次の起動から）
- 合否は tests/host/run_n2c_wide.sh の頭（I1w・I6u・I3・I6・I4、陰性 NO_REDRAW・REDRAW_VOLATILE・NO_PIN）。試験先 bd88e5c3 で 5 つとも赤
- **限界**: I3 の「教えを取り違えない」は試していない。狭い番号は v1 に鍵が無い＝ぶつかりは見えない（監査-183 ②）。drpc・pfs_repl など他の層は鍵を見ない（第三者の往復の測りだけ捨てる）。小さい鍵を作れる者は相手を引き直させ続けられる（§5 ④）。PKERNEL_PFS_DIR なしは鍵が毎回変わる＝番号も毎回変わる
