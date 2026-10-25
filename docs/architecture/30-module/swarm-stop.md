# S2 人が群れを止める道（署名つきの停止の命令）— 設計メモ
<!-- vmodel rung: PARTIAL -->
<!-- vmodel certs: [swarm-stop] [stop-console] [stop-galaxy] [stop-net] -->

ROADMAP 0-14 S2・全体の決まり 9。「故障では死なないが、人は止められる」。この道は決まり 5 の「勝手に変えてはいけない」に入る（AI が鍵の表を書き換えない、止まる条件を緩めない）。

## 形（2026-10-18 に決めた）

- **命令**: `PKSTOP`（6 バイト）＋版 1＋種類（1＝止まれ、2＝再開）＋世代 epoch（4 バイト、増えるだけ）＋署名者の公開鍵 32＋署名 64＝108 バイト。名前と版を頭に持つ（S4 の印の最初の例）
- **署名の中身**: 頭から epoch まで＋署名者の公開鍵（12＋32 バイト）。Ed25519（`sign_verify`）
- **鍵の表**: 停止の権限は **OTA・selfc の採用リスト（`sign_allow_*`）とは別の表**。コードを入れてよい鍵と、群れを止めてよい鍵は役目が違う。表はコンソール（人）だけが足せる。**表が空なら、網から来た命令では誰も止められない**（閉じた既定。手元のコンソールの停止はいつでも効く）
- **受け入れる条件（全部）**: 頭と版が合う／署名者が表にある／署名が正しい／epoch がこれまでの最大より大きい（同じ命令の流し直しを拒む）
- **止まったら**: `swarm_stop_is_stopped()` が 1。学習・教え・重みの送り出しはこれを見て止まる（次の段でつなぐ）。命令は仲間へ1回だけ流し直す（群れ全体へ。次の段）

## 段

1. **in-proc（この段）**: `arch/common/swarm_stop.c`、hosted の `tkconf stop` が `[swarm-stop]` の自己検査を走らせる。期待（作る前に固定）:
   - T1 表にある鍵の正しい「止まれ」で止まる
   - T2 表に無い鍵の命令は拒む（止まらない）
   - T3 1 バイトでも書き換えた命令は拒む
   - T4 古い命令の流し直し（epoch が小さい）は拒む。同じ epoch は T7 が確かめる
   - T5 表が空なら、正しく署名された命令も拒む
   - T6 頭か版が違う命令は拒む
   - T6b 1 バイト短い命令は拒む（同じバイト列を全長で渡すと止まる＝正しい命令を短くしたもの。監査-52 で直した）
   - T7 再開は、より大きい epoch の署名つき「再開」でだけ効く
   - T6c 1 バイト長い命令は拒む（同じバイト列を全長で渡すと止まる。監査-53 の A3 の穴）
   - T8 表の鍵で署名した知らない種類（op=3）は、止まった状態で拒み、止まったまま・epoch も進まない（監査-53 の A2 の穴: 種類の確かめが無いと op=3 が「再開」として効いた）
   - 陰性コントロール: `-DSWARM_STOP_NO_SIGCHECK` で T3 が赤、`-DSWARM_STOP_NO_REPLAY` で T4 が赤、`-DSWARM_STOP_NO_AUTH` で T2・T5 が赤、`-DSWARM_STOP_NO_LENCHECK` で T6b・T6c が赤、`-DSWARM_STOP_LEN_MIN`（短いものだけ拒む）で T6c が赤、`-DSWARM_STOP_NO_OPCHECK` で T8 が赤（後の 2 つは手元の測定だけ、CI には無い）。CI は ump-x86_64・ump-aarch64-cross の `[swarm-stop]` の step（素で全部 PASS、x86 は NO_SIGCHECK で T3 赤・aarch64 は NO_LENCHECK で T6b 赤）
2a. **コンソールと最初の従う口（2026-10-19、feat/stop-console、hosted だけ）**: `stop`（状態）・`stop now`／`stop resume`（この端末の止まり。鍵は要らない）・`stop key <hex>|clear`・`stop in <hex>`＋`stop apply`（108 バイトの命令を貼る。シェルの1行は 128 バイト）。**手元の再開は手元の止まりだけを解く。署名つきの止まりは署名つきの再開でだけ解ける**。止まっている間、シェルは `mind teach` を拒む（`mind_cmd` と同じく前方一致で読む＝`mind teachsky blue` も拒む。監査-56）。シェルの入力はコンソールだけ（stdin、Android は画面の入力欄）。試験 `tests/host/run_stop_console.sh` の S1〜S5（作る前に固定）と S6（監査-56 で足した前方一致の形）、陰性コントロール `-DSWARM_STOP_NO_OBEY` で S2・S6 赤。まだ従わないもの: 心の眠り・網から届く教え、重みの送り出し（r3 はベアにも入る＝crown が動く）
2b. **galaxy の POST /teach（2026-10-20、feat/stop-galaxy、hosted だけ）**: 止まっている間は 403 `{"refused":"stopped"}`（同意の門より前）。試験 `tests/host/run_stop_galaxy.sh` の G0〜G5（同意してから教える→止める→拒む→再開→教える、sea だけ残らない）、陰性コントロール `-DGALAXY_NO_STOP_GATE` で G3・G5 赤。/ask は止めない（読むだけ）。**署名つきの止まり（監査-59 の限界、feat/stop-galaxy-signed）**: G6〜G9（`-DSWARM_STOP_TEST_SIGN` のビルドで鍵を登録→署名つきの止まり→ /teach 403→手元の `stop resume` でも 403 のまま→署名つきの再開→教える、leaf だけ残らない）。陰性コントロールはこのビルドの上で G3・G5・G6・G7・G9 赤
2c. **網から届く教え（2026-10-20、feat/stop-net-teach、hosted だけ）**: 止まっている間、mind/teach で届いた教え（仲間の教え・pull の答え・衝突の押し返しは同じ口）を受け取らない。seq は消費しない＝再開のあとに相手の流し直しで届く。拒んだことは止まりごとに 1 行。試験 `tests/host/run_stop_net_teach.sh` の N1〜N4（作る前に固定）、陰性コントロール `-DSWARM_STOP_NO_NET_OBEY` で N2・N3 赤。まだ従わないもの: 心の眠り、重みの送り出し・受け取り（Path W）、mind/have・pull の答えを出す側
2d. **心の眠り（2026-10-24、feat/stop-sleep、hosted だけ）**: 止まっている間、DMN の休みの仕事（engram の再生・r3 の教えの蒸留・赤子の蒸留・ga_step）をしない。冬眠の止め（L2）と同じ口に足した。ga_step も止める（冬眠は費用の止めで残したが、止まりは「学ばない」の意味）。教えは保留のまま残り、再開のあとに蒸留される。試験 `tests/host/run_stop_sleep.sh` の Z1〜Z3（作る前に固定。Z0「止める前に眠る」は最初の測定で 3 本とも教えて 14 秒では眠りが始まらず外した、試験の注。見えるのは r3 の 1 行だけ、ほかの 3 つは同じ門を読むだけ）、陰性コントロール `-DSWARM_STOP_NO_SLEEP_OBEY` で Z2 赤。まだ従わないもの: 重みの送り出し・受け取り（Path W）、mind/have・pull の答えを出す側
2e. **重みの送り出し・受け取り（Path W、2026-10-24、feat/stop-pathw、hosted だけ）**: 止まっている間、mind_merge_task は rw[] を送り出さず、仲間の重みも畳み込まない（冬眠の止めと同じ所、拍は進まない）。`mind merge` も拒む。試験 `tests/host/run_stop_pathw.sh` の W0〜W4（2 ノード＋中継、作る前に固定）、陰性コントロール `-DSWARM_STOP_NO_PATHW_OBEY` で W2・W3 赤。仲間が押し出した重みの塊は pfs の複製として届きうる（pfs の層は止まりを見ない。畳み込まないだけ。止まっている間に届くかは測っていない）。まだ従わないもの: mind/have・pull の答えを出す側、仲間への流し直し
2f. **心から外へ出すもの（2026-10-24、feat/stop-out、hosted だけ）**: 止まっている間、mind_net_task の毎回の送り（仲間の want への答え・自分の教えの流し直し・衝突の押し返しの再送・自分の want・mind/have の要約）をしない。教えは消えず、再開のあとに流し直しで届く。試験 `tests/host/run_stop_outgoing.sh` の O0〜O3（B に教えて止めてから A を起こす＝A は B からしか sky を知りえない。作る前に固定）、陰性コントロール `-DSWARM_STOP_NO_OUT_OBEY` で O2 赤。見ているのは「A に届かない」だけで、4 つのうちどの口が止まったかは分けていない（監査-75: 1 つの口だけ止まりを無視させると、流し直しでは O2 赤、want への答え・mind/have では緑のまま＝見えるのは流し直しの口だけ）→ **O4・O5 を足した（2026-10-24、feat/stop-out-mouths）**: A が窓の中で `mind ask sky` して want を出す（O4、前提）＝止まった B が答えれば O2 が赤。B に A が尋ねない 2 つ目の事実（ink）も教え、A に「mind/have from node」の行が出ない（O5）。陰性コントロール `-DSWARM_STOP_ANSWER_UNGATED` で O2 だけ赤、`-DSWARM_STOP_HAVE_UNGATED` で O5 だけ赤（CI の ump-x86_64 の answer・have の腕）。want そのもの（mq_publish_wants）を止めているかは見ていない（A は止まっていない）。監査-77 の注: 止まった B の want（mq_publish_wants）を止めない版でも [stop-out] は ALL PASS＝want の口は試験の外。O5 は ink の 2 つ目の事実が無くても HAVE_UNGATED で赤（ink は要らなかった、害なし）。**→ 監査-78（2026-10-24、feat/stop-want-nc）: want の口は O6（止まった B が窓の中で `mind ask snow`＝want を出さない）と O7（再開の後 60 秒以内に出す＝陽性コントロール）で試験の中に入った。陰性コントロール `-DSWARM_STOP_WANT_UNGATED` で O6 だけ赤（手元の測定、CI の外）**
2. **[live]**: 3 ノード以上で、1 台に命令を入れると全員が止まる。表に無い鍵の命令では誰も止まらない（陰性コントロール）。**2026-10-24、feat/stop-live（hosted だけ）**: 受け入れた命令（コンソールでも網からでも）を GLOBAL の K-DDS の話題 `swarm/stop` に流し、2 秒ごとに流し直す（「1 回だけ」から変えた: 話題は LATEST_ONLY で、遅れて来る仲間もいる）。受け手はコンソールと同じ `swarm_stop_apply` を通す＝自分の表に無い署名者の命令は拒み（epoch ごとに 1 行）、同じ命令は流し直しとして黙って拒む＝輪にならない。受け口は mind_net_task の拍の頭（冬眠・止まりの確かめより前）。試験 `tests/host/run_stop_live.sh` の L0〜L3（3 ノード＋中継。A の表だけに K2 がある＝K2 の止まりは A だけ止める）、陰性コントロール `-DSWARM_STOP_NO_FORWARD` で L2 赤、`-DSWARM_STOP_NO_AUTH` で L1 赤（手元だけ）。限界: mind/teach が開けないと mind_net_task が無く、止まりも届かない。監査-75: 1 回だけ送る形に壊しても L0〜L3 は緑＝試験に遅れて来る仲間が無く、2 秒ごとの流し直しが要ることは示せていない→ **L4 を足した（2026-10-24、feat/stop-live-late）**: L2 のあとに 4 台目 D を起こし、鍵を入れてから 60 秒で止まる（流し直しでしか届かない）。陰性コントロール `-DSWARM_STOP_SEND_ONCE`（1 回だけ送る）で L4 赤・L2 緑（`late` の腕、CI の ump-x86_64）。D は L3 の再開も確かめる
2g. **再起動のあとも残す（2026-10-24、feat/stop-persist、hosted だけ）**: `PKERNEL_PFS_DIR` があれば、コンソールでの変更と網から受け入れた命令のたびに `swarm-stop.st` を書き直す（pfs_dur_write＝一時ファイル＋rename＋fsync。**pfs の ref にはしない**: ref は仲間とゴシップされ、新しい seq で上書きされる＝仲間が鍵の表を書き換えられてしまい決まり 5 が破れる）。中身は署名つきの止まり・手元の止まり・epoch・**鍵の表**・最後に受け入れた命令（再起動のあとも流し直す）。鍵の表を残さないと、再起動のあとは表が空＝署名つきの再開も拒まれ、誰も再開できない。起動時に pfs_dag_restore の後で読む。CRC か形が合わないファイルは「手元の止まり」で起動する（fail-closed、`stop resume` で抜けられる）。書くのは止まりを決める PK_NOSW の区間の中（2 つのタスクが同じ一時ファイルに書かない。人の操作のたびに数 ms）。自己検査は書かない。試験 `tests/host/run_stop_persist.sh` の R0〜R7（1 ノード・同じ dir で 5 回起こす。作る前に固定）、陰性コントロール `-DSWARM_STOP_NO_PERSIST` で R1 赤。限界: PKERNEL_PFS_DIR が無ければ RAM だけ（前と同じ）。読めないファイル（権限など）は「無い」と同じに見える。ファイルを消せば止まりは解ける（端末の持ち主はできる＝止められるのは正直なノードだけ、と同じ）。監査-77 の注: 網から受け入れた命令のあとの書き込みと、再起動後の流し直しの命令の復元は、外しても R0〜R7 が緑＝1 ノードのコンソールの試験の外（**→ 監査-78: 網から受けた止まりの書き込みは [live] の L5 で試験の中に入った。陰性コントロール `-DSWARM_STOP_NET_NO_PERSIST` で L5 だけ赤、CI の外。流し直しの命令の復元は今も試験の外**。**→ 2026-10-24 feat/stop-nofwd: [live] の L6（L3 の後に A・C・D・B を殺し、B だけを網で起こし直してから新しい E を起こす。E は 60 秒以内に resume（epoch 8）を受け入れる＝B が swarm-stop.st から戻した命令しか届かない）。陰性コントロール `-DSWARM_STOP_NO_RESTORE_FWD` で L6 だけ赤、CI の外**）。pfs_dur_write の失敗は黙る（古いファイルが残り、再起動で古い状態に戻りうる）。**→ 2026-10-24 feat/stop-save-note: 失敗したら次の状態の行の前に `[stop] could not save swarm-stop.st (the file on disk keeps the previous state)` を 1 行出す（RAM の状態は変えない、PKERNEL_PFS_DIR が無いのは失敗としない）。試験 R8（一時ファイルの名前をディレクトリにして書けなくする＝root でも失敗する）**
3. ベアメタルへ（crown が動く。判断待ちへ）

## ベアへ（R-STOPBARE の下ごしらえ、2026-10-24、コードの読みだけ・測っていない）

hosted で止まりに従わせた口（2a〜2g・[live]）は全部 `_TK_HOSTED_LIBC_` の中。ベアには swarm_stop.c が入っていない（boot/x86・boot/aarch64 の Makefile）。ベアに**ある口・無い口**:
- **無い**: mind_net_task・mind_merge_task（作るのは arch/linux の usermain だけ）＝2b・2c・2e・2f・[live] の送り出しの口、galaxy、`mind teach`（arch/x86/shell.c に無い）
- **ある（ベア x86）**: DMN の眠りの仕事（dmn.c の ga_step・engram の再生・r3 の蒸留＝`dmn_init` は arch/x86/usermain.c）、シェルの `pub` と syscall の kdds_pub（利用者のプログラムの送り出し、心ではない）、KLOAD（0-14 S3 でコンソールの門つき）
- **ベア AArch64**: DMN もシェルの教えも無い。KLOAD だけ（門つき）
- 命令の受け口: ベアには mind_net_task が無いので、[live] の受け口は既存のベアのタスク（DMN の拍など）に足すことになる
- 永続: ベア x86 には K-DDS の話題をディスクに書く persist.c がある。ただし話題は仲間と共有されうる＝決まり 5（表はコンソールだけ）に触れないかを先に確かめる。無理なら RAM だけ、と限界に書く

段の案（各段で .text の差分を測り、監査 PASS を条件に re-bless）: ① swarm_stop.c をベアに入れ、x86 のシェルに `stop` と DMN の 3 つの仕事の門（[stop-sleep] と同じ項目を QEMU で）② [live] の受け口 ③ 永続（置き場が決まれば）

① **（2026-10-25、feat/stop-bare-x86）**: boot/x86 が swarm_stop.c を組み込む。ベア x86 のシェルに `stop`（hosted と同じ swarm_stop_cmd）と `stop test`（[swarm-stop] の自己検査。ベアには tkconf が無いので共通の swarm_stop_cmd に足した＝hosted にも入る）。dmn.c の止まりの門（2d）は hosted と `_X86_PC_` で効く（ga_step・engram の再生・r3 の蒸留・赤子の蒸留（ベアでは空の関数））。冬眠の止めは hosted のまま。ベア AArch64 は dmn.c を組むが門は入らない（次の段）。試験 `tests/x86/run_stop_sleep_bare.sh` の B0〜B4（QEMU 1 台。作る前に固定）: B3 は止まっている 26 秒に DMN の idle runs が 20 以上進み、ga_step の呼び出し（steps＋skipped）が動かないこと。陰性コントロール `-DSWARM_STOP_NO_SLEEP_OBEY` で B3 だけ赤。見えるのは ga_step だけ（ベアのコンソールには engram と r3 を満たす口が無い）。まだ無いもの: 網の受け口（②）・永続（RAM だけ）

## 限界（先に書く）

- 鍵を人がどう配るか（端末ごとにコンソールで足すのか、参加のときに渡すのか）は決めていない。S1（同意制）と一緒に決める
- 鍵を失くしたときの扱い（取り消し）は無い
- 止まりと epoch は、PKERNEL_PFS_DIR が無いと RAM だけ（2g）
- **試験の穴（監査-53）**: 種類の確かめを外しても、長さの確かめを「短いものだけ拒む」に緩めても、8 項目は緑のままだった → T8・T6c を足した（10 項目、`feat/stop-holes`）
- 止まったノードは「止まった」と言うが、悪意のあるノード（改造したカーネル）は従わない。止められるのは正直なノードだけ
