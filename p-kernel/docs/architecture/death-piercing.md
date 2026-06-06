# Death-piercing — 推論の途中で死んでも、群れは答える

wave 8 / 配線③。第三のレビューの核心への回答:

> DKVA には timeout はあるが、resp_cnt==0 で E_TMOUT、起点 node0 が
> 死んだら推論ごと即死＝起点 SPOF。プロジェクト名は『死なないための
> OS』なのに、まだ一度も（推論の最中に）死なせて確かめていない。

## 何が "never dies" の最初の接地か

これまでの障害系 (SWIM 検知、replica scatter、guard respawn、p-fs 復元)
はすべて「死んだ**後**に立て直す」仕組みだった。death-piercing が初めて
接地させたのは「死んでいる**最中**でも進行中の仕事 (分散推論) が完遂する」
こと。`samples/13_survival_loop/kill_one.sh` が 3 ノード DKVA FULL の
推論ループの中で実プロセスを SIGKILL し、完遂をアサートする。CI の
`survival-loop` job が毎 push これを回す。

## 部分集約の正直さ規約 — degraded (k/n)

`dkva_infer` (arch/common/dkva.c) の待ち合わせは次の規約に従う:

1. **期待集合**: fan-out 時に SWIM が生存 (ALIVE/SUSPECT) と見ている
   自 region の peer。SWIM が DEAD と判定済みのノードは最初から待たない。
   待っている間に DEAD へ遷移したら期待から外す。
2. **早期確定**: rsum を出し得る他 region の生存ノードが居なければ、
   期待集合が満たされ次第タイムアウトを待たずに確定する (健全時の
   600ms 固定待ちも同時に消えた)。
3. **degraded の明示**: 期待 peer が欠けたまま完遂するときは
   `[dkva] degraded (k/n): completed with partial aggregation` を必ず
   出力する (k/n = 自分込みの寄与数 / fan-out 時の期待数)。**黙って
   成功にしない** — 群れが縮んだ事実は結果と同じ重さの情報である。
4. resp_cnt==0 (リモート寄与ゼロ) のときだけ E_TMOUT。呼び出し側
   (dtr.c) はローカル MHSA にフォールバックする — これは従来どおり。

SWIM が SUSPECT を広告した後は region_recompute がそのノードを region
から外すため、以後の推論は縮んだ群れに対する「期待どおりの完遂」となり
degraded は付かない。degraded が出るのは「群れの自己認識がまだ死に
追いついていない窓」だけ — それがこの規約の正直さの定義である。

## 同時多発 — 網全体で多数の問いを並行して立てる (wave 10, G1)

§5 の「同時に数百件…並行して立ち上がる」を本物にするには、推論の起点が
互いを潰さないことが要る。以前は Query が単一の共有ラッチトピック
(`dtr/dkva/q`) を使っており、異なる起点が同時に問うと後の Q が前の Q を
**上書き**して、上書きされた側は誰も応答しなかった — 網全体で in-flight な
問いは実質 **1 つだけ**という隠れた直列化点 (= 隠れた中央) だった。

修正は三段:

1. **per-origin Query** — Q を起点ごとの `dtr/dkva/q/<origin>` へ発行する
   (resp/rsum と同じ per-source 方式)。各起点が独立ラッチを持つので、
   同時に問うても互いを潰さない。responder は全 origin の `q/<o>` を購読する。
2. **origin で取り違えを消す** — `DKVA_RESP_PKT` に `origin` フィールドを
   足し、requester は自分宛 (`origin == 自ノード`) の応答だけ受理する。
   req_id が起点間で衝突しても origin で曖昧さが消える。
3. **応答スロットの時間多重** — 1 つの responder の `resp/<n>` スロットも、
   複数起点が同時に問えば起点間で上書きし合う。responder は pending な
   各 origin の応答を **ラウンドロビンで再発行** (1 反復 1 件) するので、
   どの起点も自分のポーリング窓内で自分宛の応答を取りこぼさない。

これで `samples/13_survival_loop/concurrent_infer.sh` シナリオ A の通り、
node1 と node2 (さらに node3) が**同フレームで**推論を発行しても両方が完遂し、
各起点の出力指紋 (fp) は単独実行のベースラインと一致する (混線しない)。

容量は据え置きで足りる (`dkva_init` のコメント参照): dkva の pre-open は
1 ノードあたり トピック 96 / ハンドル 192 / **セマフォ 0** (全て
`kdds_open_poll_scoped` の zero-sem)。per-origin Q を足してもセマフォ枠は
増えない — 旧実装が blocking open で浪費していた 130 個を poll 化で 0 にした。

## region 横断の正直さ — 他 region が欠けても黙って成功にしない (wave 10, G2/G8)

degraded(k/n) は当初**自 region の期待集合しか数えていなかった**。他 region の
寄与は coordinator が `rsum/<rid>` で運ぶが、その rsum が (coordinator 死亡 ＝
G8、パケット欠損で) 届かなくても degraded が付かず、縮んだ群れを黙って成功
扱いにしていた — death-piercing の正直さ規約の region 横断の穴。

修正は期待集合を二層にした:

- **expect[n]** : fan-out 時に生存と見ている自 region の peer (直接 partial を待つ)。
- **rc_expect[c]** : 応答すべき他 region の coordinator `c` (rsum を待つ)。
  どの remote ノードがどの region に属すかは、各ノードが self-beacon で広告する
  `region_id` を world マップ (`world_peer_region()`) から引いて束ねる。中央の
  真実ではなく**受信したゴシップだけ**から組む (NO-CENTRAL)。gossip 未着なら
  各 remote ノードを 1 region とみなし保守的に数える (黙って成功にしないため)。

degraded の分母は `1 (自分) + 期待した region peer + 期待した他 region` となり、
**coordinator の消失 (G8) もこの分母に乗る**ので、その region の寄与が無音で
消えることはなくなった。SWIM が DEAD と判定した region は待たない (ハングしない)
が、欠損として degraded に正直計上する。`concurrent_infer.sh` シナリオ B で、
2 region 構成の他 region をまるごと kill すると `degraded (2/3)` が必ず出て完遂する。

G8 のストレッチ (coordinator 死亡時に region 内の次点が in-flight rsum を
肩代わりする) は未実装。今は「失われた」と正直に言うところまでで、これは
下記「残課題 — 自動引き継ぎ」と同じ系譜の次の波。

## 起点の特権を消す

- `dkva_infer` 自体にノード ID 前提はもともと無い (src_node は
  `drpc_my_node`、responder は全ノードで常駐)。node0 前提だったのは
  **コメントと運用**だけで、これらは潰した (dkva.h / dkva.c)。
- シェル verb `dkva infer [a b c d]` (dkva_cmd) を追加。Q を引数から
  決定論的に合成するので、**どのノードから発行しても同じ問い**になる。
  kill_one.sh シナリオ B はこれで「起点を殺す → 生存ノードが同じ問いを
  発行 → 完遂」を示す。起点はただの呼び出し元であり、特権ではない。
- 既知の残置: dtr.c の FULL 経路には「偶数 node id だけが requester に
  なる」heuristic (`drpc_my_node % 2 == 0`) が残っている。これは役割
  分担の作法であって到達性の制約ではない (奇数ノードも `dkva infer` で
  起点になれる)。dtr.c は別隊の領分のため今回は触っていない。

## 残課題 — 自動引き継ぎ

起点が死ぬと、その推論の**結果**は失われる (問いはどこにも永続化されて
いない)。今回示したのは「他のノードが即座に同じ問いを発行して完遂できる」
までで、自動引き継ぎ (in-flight な問いの複製・生存ノードによる自動再発行・
結果の届け先の付け替え) は意図的にスコープ外。実装するなら問いを K-DDS
の latched topic か p-fs に置き、SWIM の死亡イベントで再発行をトリガー
する形になる — survival-network.md の「考える器官」の文脈で次の波。

## テストと CI

- `samples/13_survival_loop/kill_one.sh` — シナリオ A/B/C、失敗時
  exit 非0、タイムスタンプ付きログ。詳細は同ディレクトリの README。
- `samples/13_survival_loop/concurrent_infer.sh` — 同時多発 (G1) と
  region 横断の正直な degraded (G2/G8) を実証。シナリオ A: 複数起点の
  同フレーム推論が両方完遂・fp 一致。シナリオ B: 他 region 全滅で
  `degraded (k/n)` を必ず出力して完遂。
- `.github/workflows/ci.yml` の `survival-loop` job — ubuntu 上で
  boot/linux_x86_64 + relay をビルドし kill_one.sh を回す (15 分制限)。
