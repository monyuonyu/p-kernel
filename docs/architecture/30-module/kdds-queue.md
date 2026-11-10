# kdds: 送り元ごとに取りこぼさない受け（R-KDDSQ の設計）
<!-- vmodel rung: PARTIAL -->
<!-- vmodel certs: [kdds-queue] -->

2026-11-08。**第 1 段は hosted だけ**（ベアは同じコード＝crown 不変、ベアへ広げるのは次の段で re-bless）。判断待ち R-KDDSQ（11-07 05:08）に 24 時間返事が無く、おすすめで設計から始める。第 1 段の実装は同じ回（11-08 04:05、feat/kdds-queue-design の上）、設計と実装の監査は別の回。

## 1. いまの形と消え方（kdds.c）

- 1 つの話題に枠が 1 つ（KDDS_TOPIC.data）。kdds_pub_core（251〜）は上書きして data_seq を進め、kdds_rx（491〜）は上書きするが **data_seq を進めない**
- kdds_sub（369〜）は RELIABLE・LATEST_ONLY で data_len > 0 なら**毎回いまの 1 枠を返す**。poll の読み手（pfs_repl の ann・want・sync、pfs_dag の ref、mind/have など）は 1 tick に 1 回読み、中身で重複を落とす
- 同じ tick に 2 通着くと前の 1 通は読まれずに消える（run_relay_failover の赤、✅ 11-07 04:05 の回）

## 2. 形（決めたこと）

- **kdds_sub の振る舞いは変えない**（ユーザー空間との約束ごと）。新しい呼び `kdds_sub_next(handle, buf, len)` を足す: その handle がまだ読んでいない電文を古い順に 1 通返す。無ければ 0（待たない）
- 共有の小さな待ち行列 KDDS_Q_N = 32 枠（話題の番号・seq・長さ・192 B）＝約 6.3 KB。話題ごとの枠は足さない（400 話題で約 233 KB＝ベアに重い）
- 上書きの前に: その話題に `kdds_sub_next` で読む handle があり、まだ今の seq を読んでいない handle が 1 つでもあれば、今の中身を待ち行列へ退避。待ち行列は話題ごとの自分の数え（kq_tseq、pub と rx の両方で進む）で順を見る。**data_seq は使わない**（kdds_rx は進めず、replica.c がそれで新しい方を選ぶ＝rx で進めると複製の勝ち負けが変わる。11-08 の実装で気づいて直した）
- handle ごとに読み位置（最後に読んだ seq）。待ち行列が溢れたら一番古いものを捨て、話題ごとの「捨てた数」を数える（第 1 段は kdds_queue_dropped だけ、`kdds stat` への表示はまだ）
- 移すのは複数の送り手が 1 つの話題を使う読み手から 1 つずつ（最初は pfs_repl の ann）。移した読み手は中身での重複落としを残す（seq は送り手をまたいで同じ意味を持たない）
- 守り: 退避と読み位置の更新は PK_NOSW（いまの枠と同じ）

## 3. どう測るか（コードより先に固定）

- 試験先 `kdds queue`（hosted の自己試験）: 同じ話題へ kdds_rx を続けて 3 回 → `kdds_sub_next` が 3 通を古い順に返し、4 回目は 0。`kdds_sub` は今までどおり最後の 1 通を何度でも返す。読む handle が 2 つなら両方が 3 通ずつ。33 通で捨てた数 1 以上。今の木では kdds_sub_next が無い＝建たない（赤）
- 陰性 `-DKDDS_NC_NO_QUEUE`（退避しない）で 2 通目以降が消えて赤
- 生きた試験: run_relay_failover を 2 回ずつ（移した後）
- 約束ごとの試験（tkconf）とベアの 2 つの build が通ること。kdds.c はベアに入る＝crown が動く→ re-bless を判断待ちに（24 時間の決まり）

## 3b. 第 2 段（2026-11-08、hosted だけ）

pfs_repl の ann の受けを kdds_sub_next に移した（`pfsr_ann_drain`、1 tick に最大 8 通、送り手ごとの seq の重複落としは残す。ベアは kdds_sub のまま 1 通＝crown 全桁同じ）。試験 `kdds ann`、陰性 -DPFSR_NC_ANN_LATEST。run_relay_failover は移す前と後で 2 回ずつ PASS（この試験は ann を通らない＝回帰の確認だけ）。CI: UMP x86_64 に `kdds ann` と陰性 2 本（KDDS_NC_NO_QUEUE も）。限界: task が drain を呼ぶことは cert が見ない。ann を生きた群れで通す試験はまだ無い

## 3c. 第 3 段（2026-11-10、hosted だけ、mind/have）

CI 42656744519 の X3H 赤（first-sky-hint-not-299@4）から。mind/have は全員の要約が 1 枠に入り、poll は最後の 1 通だけを読んでいた。old 3 台と 300 が同じ周期で送ると 300 の要約が読まれる前に上書きされ続ける（容器で node4 が一人で 299 のヒントを待った秒数: 1〜24 秒、周期は約 2 秒）。mh_poll を `mh_drain`（kdds_sub_next、1 tick に最大 MH_PER_TICK 16 通）に移した。mind/have の節は全体が hosted＝ベアは変わらない。試験 `nodes mhq`（3 台の要約が 1 tick に → ヒント 3）、陰性 -DMH_NC_POLL_LATEST（ヒント 1）。check_callsites に mh-drain。限界: mind/hist（mhi）は 1 通読みのまま。自分の要約も待ち行列に入る（読んで捨てる）

監査-175（2026-11-10）: 限界つき PASS。同時 13 組で一人の秒数は直しなし 1〜19（19 が 2 回）対 直し 1〜3、陰性 -DMH_PER_TICK=1 も赤。CI の 120 秒は手元で再現しないまま（26 本 PASS）。pfs の ann との枠の食い合い（kdds_queue_dropped）は測っていない。読み手を移す前の旧版は毎 tick 同じ要約を読み直していた（同じ要約で 2 度目のヒント・押し返しは出ない）が、送り手が 2 秒ごとに送り直すので生きた試験 7 本は緑

## 3d. 第 4 段（2026-11-10、hosted だけ、mind/teach と mind/want、X3F）

監査-175 の容器（重い負荷）で X3 の fire が 26 本中 3 本赤（直しなしでも）。赤の 3 本とも 260 は初回の学習の直後に fire を want したのに、fire は X3H の後（120 秒の窓の外）に届いた。abB5: node 4 が 260 の want に fire で答えた数行後に別の want へ snow を同じ mind/teach に送り、260 は snow（見た seq＝黙って落とす）だけを読んだ。答えは同じ want の世代には一度だけ。abA8: 44 は 260 の最初の want を一度も表示していない（ほかの want に上書き）。初回の学習が 71〜75 秒かかり窓を食うことも重なる。
形: 1 tick に読むのは、まだ読んでいない一番古い 1 通（kdds_sub_next）、無ければ今までどおり枠の読み直し（kdds_sub）。1 tick 1 通・本体はそのまま・枠の読み直しが再送を動かすのも同じ・同じ電文の 2 度目は seq で落ちる。試験 `nodes mtq`・`nodes mqq`（3 通続けて → 1 2 3 3、古い読みは 3 3 3 3）、陰性 -DMT_NC_TEACH_LATEST・-DMQ_NC_WANT_LATEST、check_callsites mt-rx-read・mq-rx-read。ベアは kdds_sub のまま。限界: 止まっている間・冬眠の間に待ち行列に入った電文は読んで捨てる（送り手の再送に任せる、前と同じ）。共有の 32 枠を pfs の ann・mind/have と分け合う

## 3e. 第 4 段の 2（2026-11-11、hosted だけ、答え手の表、X3F）

3d だけでは X3F は消えなかった（11-10 の容器 14 組で X3F の形は master 1・直し 1）。直しの側では 4 と 44 が 260 の want を受け取ったのに答えていない。答え手の譲り合い（stagger）・最初の答え・再送は「その asker の want を読んだ tick」にしか進まず、待ち行列から 1 度読まれた want は stagger の 1 歩で使われて二度と読まれなかった。
形: asker ごとの行（MQ_ASK_STATE）に最新の want（org・keys）を持つ。mq_drain が 1 tick の want を古い順に最大 MQ_PER_TICK（16）通読んで行に入れ（無ければ枠の読み直し）、mq_step_rows が毎 tick 全部の行を進める: stagger は行ごとに 1 つ減り、1 tick の送りは今までどおり 1 つ（答えか再送）、順番は行の輪番。1 行の中身（答えは 1 世代 1 度・1-3c の再送・-DMQ_REDRIVE_LAST_ONLY）は前の本体のまま。どの asker への最初の答えも、どの asker への再送より先（2 回の見回り。陰性 -DMQ_NC_REDRIVE_FIRST）。11-11 の容器（重い負荷）で、直しの node 4 が 3 人の snow に順に答えている間に 260 の fire の番が窓に間に合わなかった形を見たため（時刻の無いログからの読み）。試験 `nodes mqq` の [mqa]（3 人の want が 1 tick に来て stagger 2 → 8 tick で 3 人とも最初の答えが 1 度）、陰性 -DMQ_NC_STEP_READ_ONLY（3d の形＝読んだ行だけ進む）と -DMQ_NC_WANT_LATEST で赤。check_callsites mq-poll・mq-drain。ベアは前のまま。限界: 死んだ asker の行は残る（再送は予算で止まる、枠のときと同じ）。[mqa] の答えと送りは偽物（mq_answer_build と線は通らない）

監査-177（2026-11-11）: 限界つき PASS。[mqa] は「1 tick に何通でも送る」壊しで緑だった（送り 5→8）ので、1 tick の送りが 1 以下を足し、陰性 -DMQ_NC_MANY_SENDS（最大 3 通で赤）。残る行の害は 1 行あたり 再送 20×答えた数＋40 で止まり、最初の答えは再送より先なので生きた asker の最初の答えは食わない（満ちた asker は空の want を 1 度送る、それを失うと古い keys が残る）。build_all と同時の重い負荷では X3 は両方の腕 4/4 赤＝結果の差は言えないまま。stop_outgoing の O4 は master と直しが同じ回に赤（3 回中 1）

## 4. 限界

待ち行列は全部の話題で共有＝1 つの話題が溢れさせると他も古いものを失う（捨てた数で見える）。順番は 1 台の中での着いた順で、送り手をまたいだ順序は約束しない
