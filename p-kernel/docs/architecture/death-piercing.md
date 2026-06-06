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
- `.github/workflows/ci.yml` の `survival-loop` job — ubuntu 上で
  boot/linux_x86_64 + relay をビルドし kill_one.sh を回す (15 分制限)。
