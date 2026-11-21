# 貼れる瞬間のカード（全体の決まり 10）の設計メモ（2026-11-21、docs だけ・未実装）

> 「あなたが教えた言葉が、知らない誰かの問いに答えました」を、本人が選んで外の SNS に貼れるカードに。方舟の中に数・順位は入れない。

## 1. 今あるもの（master 9844f12e を読んだ）

- 教えの包み MT_TEACH_PKT（arch/common/include/dtr.h）は origin_node と prov_head（教えた人の self/prov の id）を運ぶ。受けた B は (自分の seq, origin, prov_head) を mt_rprov_put で覚える（r3_incontext.c 6023・6065）
- B が問いに答えるとき（m_ask、4231〜）、事実が他の台からなら mt_rprov_find で教えた台が分かり、コンソールに「taught by node N」と出す（4320〜4333）。**A に知らせる道は無い**。数えるのは B の中の salience だけ（網に出さない、4304）
- 答えが中継（mind/want への答え）で来ても origin は元の教えた人のまま（5233〜）

## 2. 決めたこと（分かれ道はおすすめで決めた）

1. **B→A の知らせ「mind/used」を足す**。中身は origin の fact_seq と prov_head だけ。**問いの文・B の handle は入れない**（B の台の番号は網の上で見えてしまう＝限界）。B は同じ事実について**最初の 1 回だけ**送る（送った印を覚える）。B は `mind used off` で送らない
2. **A は自分が教えた事実の知らせだけを受ける**（prov_head が自分の self/prov の鎖にあるか、fact_seq が mq_lprov にあるか）。合わないものは捨てる
3. **A が残すのは事実ごとに「最初の瞬間」1 つ（日時だけ）**。2 回目からは捨てる＝数を作らない。置き場は PKERNEL_PFS_DIR の moments（記録 ark/rec とは別、原本ではない）。既定は受ける（返すのは仕組みからの反応、決まり 10）。`mind moments off` で受けない
4. **ページ**: `GET /ark/moments.json`（合言葉の後ろ、数の欄なし）と画面の 1 行「あなたが教えた『K → V』が、別の端末の問いに答えました（日時）」。「カードにする」で canvas から PNG（言葉と一文と日付だけ。handle は本人が選んだときだけ、台の番号は入れない）。貼るのは本人（保存・共有）
5. hosted だけ（r3_incontext.c はベアにも入る＝作る回は `_TK_HOSTED_LIBC_` の中に置き、crown を測る）

## 3. 合否（作る前に固定。`tests/host/run_moment.sh`、2 台＋中継、ページは tests/web）

- **M1** A が K→V を教え、B が K を問い A の事実で答える→ A に `[moment] first: K` が 60 秒以内
- **M2** B が K をもう 2 回問う→ B の送った知らせは 1 通、A の moments は 1 行のまま
- **M3** 知らせの包みに問いの文と B の handle が無い（B に handle を名乗らせて、A の受けた包みを走査）
- **M4** B が自分で教えた事実で答えた→ 知らせなし
- **M5** A が K を教えていないのに偽の知らせ（K と適当な prov）を送る→ A は残さない
- **M6** `mind moments off` の A は残さない／`mind used off` の B は送らない
- **M7** moments.json と画面に数・順位の語が無い（A5 と同じ語の一覧）。カードの文に台の番号が無い
- 陰性: `-DMOMENT_NC_EVERY`（毎回送る→M2 赤）・`-DMOMENT_NC_LEAK`（handle を入れる→M3 赤）・`-DMOMENT_NC_ANY`（照合しない→M5 赤）

## 4. 限界

- B の台の番号は網で見える。知らせに署名は無い＝偽の「瞬間」は作れる（数が無いので得は小さい、照合で自分の教えに限る）
- A が止まっている間の知らせは失う（送り直さない）。最初の 1 回を逃せば、その事実の瞬間は残らない
- 「知らない誰か」かは確かめない（同じ人の 2 台目でも瞬間になる）
