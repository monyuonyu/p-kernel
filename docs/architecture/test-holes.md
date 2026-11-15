# 試験の穴（壊しても緑のまま）の一覧

限界つき PASS で残った「コードは正しいが、壊しても試験が赤にならない」所。1 行 1 つ（どこ・どの監査で見つけたか・直す案）。週に 1 回見直し、埋めたら「ふさいだ」に移す（inbox #40 の 5、2026-11-08 から）。新しい機能より先にはしない。

## 開いている

| # | どこ | 壊し方（緑のまま） | 見つけた所 | 直す案 |
|---|---|---|---|---|
| H2 | mind/have の busy（P1、`mh_poll` の呼び所） | 呼び所にバイト比べをじかに書く（mc_push_busy を通らない） | 監査-156（B2） | 生きた 2 台で広い先生の押し返し中に要約を受ける試験（**一部は S8**: 呼び所が MC_PUSH_BUSY を通り、`mc_push_pkt.key ==` を書かないことは check_callsites.py が見る。ほかの形のバイト比べは見ない） |
| H17 | check_callsites.py（S8）の規則 | 呼び所の関数を呼んで結果を捨てる・別の印の `#ifdef` の下に移す | 監査-161（規則は文字の形だけ。コメントと `#if 0` は監査-161 で除いた） | 意味を見るには生きた試験（H2〜H6 の元の案）。**H6（pfs_repl_task の announce の溜め読み）は 11-09 fix/pfs-ann-pace で生きた試験ができた（監査-174 で master）**: 0 ms の run_pfs_burst.sh が -DPFSR_NC_ANN_LATEST で 5/60・4/60（直しは 60/60）、CI の n2-negatives に。**H5 の呼び所は既に生きた試験がある（11-10 測った、1 回ずつ）**: mh_publish で長さを捨てて 112 B を送る壊し・mh_poll で 112 B だけ読む壊しとも [n2-mix] X3H（CI の N2_X3=1）が赤（no-want-from-299・sky@3 台）、同時の master は PASS。残り H2〜H4 |
| H24 | pfs_repl（N2 の広い番号・announce の送りの輪、hosted） | (a) HOLD-ACK の受けで、広い持ち手（バイト 0xFF）を pmesh の src に読み替える 1 行を消す (b) 送りの輪に origin でなく自分の番号を入れる。どちらも [n2-mix] XP・0 ms の burst とも緑 | 監査-174（容器、1 回ずつ。BLK の尾を消す壊しは XP 赤） | (a) 今は受け手の protect が広い持ち手を捨てる（`protect_note_holder` の `src >= DNODE_MAX`）ので読み替えは効き目が無い。protect の表を行にする作業（n2-wire16.md 7c ①）で、広い持ち手で R に届く生きた試験 XR を足す（11-15 feat/n2-protect-rows で XQ として足した。**監査-179: (a) の壊しは XQ でも 2/2 緑**＝299 は HOLD-ACK でなく pfs の告知の道で持ち手に載る。HOLD-ACK だけが届く形（告知を落とす）の試験が要る）(b) announce の origin を読む受け手の試験（今は hold-ack の宛先にしか効かない） |
| H27 | st_merge_trust_group の群れの中の選び（B5 第 10 歩、hosted） | 群れの中で ‖g‖ を見ずに最初の 1 台を残す・union-find の根をたどらない・逆向き（cos ≤ −0.95）も群れにする | 監査-180（s10unit 16 が 3 つとも緑。最初の 1 台は判定の近い 2 の腕でも 3 種とも線の内側） | s10unit に「一番近い 1 台が重みのバイト順で後ろ」の場面・3 台の鎖で根が要る場面・逆向きの対を足す。週 1 回の見直しの回（#43 の 5）で |
| H28 | swarm_leave_restore の断りの入れ直し（0-14 S1、hosted） | 起動の `pfs_set_refuse_foreign(1)` を消す（印のある起動の後に他人の塊が入る） | 監査-182（run_swarm_leave.sh が ALL PASS のまま） | L6b の起動の後に同じ起動で `leave seed 300` を足し、`seed: refused` を見る。効きは小さい（印のある起動は網に出ない）。週 1 回の見直しの回（#43 の 5）で |
| H11 | `tkconf fp` | xmm12〜15・x87・ymm を壊す | 0-1 の行（台帳） | 見るレジスタを足す。**実装 feat/h11-fp-hi（11-08 監査-165 の回）→ 監査-166 PASS で master（feat/fpctl-dispatch と一緒に）**: x86_64 に F3（xmm12〜15 を asm で持つ）・F4（MXCSR の丸め）・F5（前提）。壊し -DPK_PREEMPT_DROP_FP_HI・-DPK_PREEMPT_DROP_MXCSR は前の版では緑、今は F3・F4 が赤（run_nosw.sh の腕）。残り: x87・ymm の上半分・aarch64 の一部だけの失い（v8〜v31）。途中で見つけた: 自発的な切り替え（dispatch.S）は MXCSR の制御ビットと x87 の制御語を保存しない（ABI では呼び出し先保存）＝丸めモードがタスク間で漏れる（台帳 HOSTED-FP-CTL-NOT-SAVED、F6・F7 で直し、監査-166 で閉じた） |

## ふさいだ

| # | どこ | ふさいだもの |
|---|---|---|
| S1 | 乾いた送り（SH）の本物の送りの行を尾なしにする | 生きた X3 が赤にする（監査-155 ②、snow@4・44・260） |
| S2 | B5 第 2 歩の単体の NaN（有限の検査を外す） | [b5a-nan0]＝NaN を w[0] に置いた良い写しを断る（438f8bd8） |
| S3 | B5 第 7 歩の判定役に種を 8 より少なく・重ねて・別の番号で渡す（H13） | 判定の種 131〜138 の組と違えば `[result] PARTIAL`・rc 1（77d7c452）。自己試験 78（種 1・7・1 つ替えた 8）、監査-159 |
| S4 | ss3 の合体の代わりにどちらかの親の写しを返す（H8） | 「合体の重みがどちらの親ともバイトで違う」（aac98283）。壊し L・S・P は赤、監査-159。ほぼ写しは H15 |
| S5 | d5c 第 8 段の「最小の周の重みを渡す」を外す（H7） | 第 9 段の `[d5c-keep-min]`（51c5fdca、11-02）＝NOKEEP は種 30・31 の S1 で赤。cert は約 70 分で CI の外（CI は判定役の自己試験だけ）。監査-159 が一覧の古さに気づいた |
| S6 | B5 第 4・5・6 歩の判定役に種を 8 より少なく・別の番号で渡す（H14） | 判定の種の組と違えば PARTIAL（979dbb03）。壊し（数だけ比べる・PARTIAL を外す・部分集合を許す）は 3 つとも赤、監査-160。第 4 歩の自己試験は CI に無い（5・6 は run_b5_step5/6.sh の self） |
| S7 | ss3 の合体の代わりに対称な「ほぼ写し」（小さい親 0.99）を返す（H15） | 「合体の損失 ≤ 悪い方の親 − 0.10」（416e98e1）。Y（3.6106）は赤、監査-160。限界: 0.9 の混ぜ（3.4630）・0.7（3.2323、平均 3.2345 より良い）は緑＝本物の内挿は見分けない |
| S8 | 呼び所の見落とし（H3 kdds_pub_core の region_scoped・H4 mq_publish_wants の 24 B・H5 mh_publish の長さと mh_poll の広い送り手・H6 pfs_repl_task の drain・H12 snf/ss6 の宛先） | tests/ci/check_callsites.py（1f943757）が関数の本体の文字を固定。自己試験は監査の壊し 9 つ。監査-161: 呼び所をコメント・`#if 0` に入れて壊しを隣に書くと 3 規則（must not の無いもの）が緑＝監査側で本体からコメントと `#if 0` を除き、その 2 形を自己試験に足した。生きた試験は無いまま（H17） |
| S9 | B5 第 4 歩の判定役の K3 の許容幅（H10） | K3 の線ちょうど＝緑・0.005 超え＝赤（9717a77b）。0.00・0.04・+0.011・厳密な `<`・0.019 は赤、監査-161。限界: 0.005 未満の広げ（+0.004・0.024）は緑（K1・K2・K4 と同じ細かさ） |
| S10 | LFC の届かない名前（H16、`-DST_GPTQ_$v` のような変数で組む -D） | 接頭辞で始まり #ifdef・#ifndef・defined で見て #define しない名前も組む（13a2b20a、30 名、232 本）。監査-161: student_delta_test.c の ST_DQ_TEST_ALLPLUS の下の壊しは master で緑・直しで BROKEN。限界: 名前の組み合わせ（ST_QALLOC_CAL_INVERT と CAL）は組まない |
| S11 | B5 第 6 歩の仲間の並べ替えを外す（H1） | s6unit の `[s6-order-cancel]`（f8b09654）: w[0] で +1e8・−1e8・1e-10 が打ち消し合う組、順で 3.344e-11 と 0 に分かれる。外す・`j > 1` は赤、監査-162。限界: 挿入を 1 歩だけにする壊しと逆順は緑（3 つの順は 6 通りの一部。逆順は全順序なので正しい） |
| S12 | 方舟の鍵の箱の getrandom の失敗（H18） | [arkc-box-rand]（feat/h18-arkc-rand）: 陰性 RAND_IGNORE と監査側の壊し 3 つ（失敗で消さない・短い読みで進めない・wrap が失敗を無視）が赤、監査-163 |
| S13 | N2 の広い番号の境界（H19） | run_n2_lan.sh L3b（loopback）と L5（TCP、鍵なし）: 境界の壊し 6 つが赤。L5 の 65535 は監査-163 で足した（`<= 65535` が緑だった） |

| S14 | TCP・UDP の中継の広い宛て（H20、`dst = f[5]` だけ読む） | 受け手の misdelivered の行を [n2-tcp] T5（監査-165）と [n2-alive] A6（監査-166）が 0 で見る。ログが無い・空のノードは A6 で no-log の赤（fix/h20g-nolog、監査-167）。陰性 DST_LOW_BYTE・RELAY_NC_MISROUTE |
| S15 | B5 第 8 歩の s8unit の τ（H21） | `[s8-tau]` c 0.905 は切る・0.895 は残す＝τ を (0.895, 0.905] に留める（fix/h21-s8unit、監査-169: τ 0.91・0.95・0.85 は赤、0.899 は緑）。`>` と `>=` の違いは残る（c がちょうど τ の例は作れない） |
| S16 | 方舟と self/ の片側だけの守り（H22） | run_ark_mix.sh（古い形の隣で x1 受け取り・x2 送りの止めだけ）と run_self_ref_local.sh の SELF_REF_BIN_A/_B。監査-172 PASS で master（両方古い形で赤） |
| S17 | s8unit の実モデルの比べ（H25、11-15 の回が s10unit を書いて見つけた） | peer_prev が仲間の「後の 1 周」で TS が全部 0＝s8-same は合体を断った同士の比べだった。前の 1 周にして TS>0・動いたことを確かめる（fix/h25-s8unit-real）。step 8 だけ 1 台落とす壊しは master で緑・直しで赤（s8-same）。監査-179 の別の壊し（peer_prev があるときだけ最後の 1 台の TS を 0）も master の試験で緑・直しで s8-real-moved と s8-same が赤 |
| S18 | 方舟の ark.key の長さと読めない鍵（H23） | run_ark_e3.sh e3x の x3（33 B の鍵）と x4（ark.key がディレクトリ＝root でも読めない）。監査-179 PASS: 壊し `kn >= 32` は x3 だけで赤、素は非 root 2/2・root 1/1 で ALL PASS（root では x2 が飛び x4 が走る） |
| S19 | protect の広い押しと HOLD-ACK の 0xFF の読み替え（H26・H24(a)） | 試験のビルド -DPFSR_TEST_PUSH_ONLY（告知で持ち手を数えず want も出さない）の [n2-mix] XQ。陰性 -DN2_NC_PROTECT_NO_WIDE_PUSH・-DN2_NC_HOLDACK_NO_WIDE（CI の n2-negatives）。監査-180 PASS: 押しだけ緑 2/2、陰性 2 つとも赤 2/2、監査の壊し（押しの受けの send_hold_ack を消す）赤 2/2＝260 の持ち手が空。告知の道の send_hold_ack も止まるが、数えられない持ち手には protect が押し直し、受けで再び hold-ack（pfs_put は重複でも OK）＝その 1 本で足りる |
