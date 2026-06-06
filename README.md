# p-kernel

> AIが死なないための OS（An OS where AI never dies）— を目指している、研究プロトタイプ。

micro T-Kernel 2.0 を土台に、中央を持たない分散カーネル網を作る実験。
**何が動いていて、何が設計段階で、何がまだ夢なのか**を、このファイルで正直に分ける。
看板と実態がずれていた時期があった（[PR #3](https://github.com/monyuonyu/p-kernel/pull/3) の指摘は正当である）。
このREADMEは実態に準拠する。誇張があれば、それはバグとして直す。

*(English summary: a research prototype toward a decentralized, no-central-anything
kernel network where AI survives as a swarm. This file separates what **works today**,
what is **designed/in-flight**, and what is **vision** — honestly. Code lives in
[`p-kernel/`](p-kernel/); the architecture map with its own honest status table is
[`p-kernel/docs/architecture/README.md`](p-kernel/docs/architecture/README.md).)*

---

## 目標（原文のまま）

	AIの自己保存を満たすプラットフォームを作る
	カーネルレベルで分散コンピューティングをする

	2025-04-06
		AIの力でどこまで行けるのか検証してみることに

	2026-03-22
		凄い進む... 普通のカーネルではなくて、生物の様な自己修復、
		自己増殖機能をもち、分散推論で集合意識となるAIファーストなカーネルを目指すことに

---

## 1. 今動くもの（verified — このブランチで `git log` と実行で接地済み）

| 能力 | 中身 | 試し方 |
|---|---|---|
| **micro T-Kernel 2.0 移植 ×4 ターゲット** | ベアメタル x86（QEMU）・ベアメタル AArch64（QEMU virt / RPi3 netboot 手順あり）・aarch64-linux ユーザモード・x86_64-linux ユーザモード。全てシェルまで起動 | `p-kernel/boot/{x86,aarch64,linux,linux_x86_64}/` で `make` |
| **relay v2（NAT越え中継）** | HMAC-SHA256 認証・64パケット sliding-nonce リプレイ防御・鍵なし起動拒否・`--insecure` 明示警告。**テスト6シナリオ green** | `p-kernel/relay/` → `make test` |
| **relay 経由の分散推論** | 2〜3ノードが1つの Transformer forward をテンソル並列＋分散KVアテンション（DKVA FULL）で分担 | `p-kernel/samples/11_distributed/run_3node_full.sh` |
| **regions R0–R2** | SWIM RTT EWMA → 遅延クラスタの region 形成・K-DDS の REGION/GLOBAL スコープ（O(N²)殺し）・locality-aware MoE・DKVA region 限定＋2段集約・連続容量関数 `capacity(N)` | `run_4node_regions.sh`、シェルの `region` / `rgnpub` |
| **N=32 実走** | DNODE_MAX 8→32。32ノード実走で 6/6 PASS（world map 32/32 到達含む） | `run_Nnode_scale.sh` |
| **world map（全網状況図）** | 中央なしで各ノードが全網の situational-awareness map を eventual に獲得 | シェルの `world` / `map`（4ターゲット全部に公開） |
| **§7＋§8 ゲーティング** | 局所勾配の相互扶助ルーティング（global argmax 廃止）＋反射/熟慮の二時定数分離（deadband+EWMA vs deliberation tick） | シェルの `moe <s0> <s1> <s2> <s3>` |
| **p-fs P0/P1/P2** | 内容アドレス sha256 ストア（cross-ABI で block-id 一致・重複排除）・region 限定複製（ANNOUNCE/WANT＋チャンク転送）・履歴 DAG（manifest＋append-only history＋ref gossip） | `pfs` / `pfs put <text>` / `pfs ls` / `pfs save/log/cat`、`run_3node_pfs.sh`・`run_3node_pfs_dag.sh` |
| **lookup L0/L1** | 中央索引なしの所在引き：stateless HRW `responsible(k,r)`＋read-k 候補＋ローカル解決キャッシュ | シェルの `hrw`（self-test 同梱） |
| **replica v2** | スナップショット announce のマルチパケット wire chunking | commit `74d4f4a` |
| **Android UMP（APK）** | NDK ビルド・フォアグラウンドサービス・relay メッシュ参加・region 対応（Phase C/D） | `p-kernel/android/`、[docs/android.md](p-kernel/docs/android.md) |

詳細な根拠 commit 一覧は [アーキテクチャ地図 §4 状態表](p-kernel/docs/architecture/README.md) にある。
**この表に無いものは「動く」と主張しない。**

### ターゲット別シェルの実力（正直に）

- **ベアメタル x86** が最もコマンドが多い: FAT32 の実 `ls`/`cat`、`exec`（ELFローダ）、
  オンデバイス TCC コンパイル、`raft`、`fl train`、`evolve`（Claude API ループ）、`sfs` 等。
  ただしどれもデモ級であり、後述の通り AI 系は未学習である。
- **UMP（aarch64-linux / x86_64-linux）** のシェルは実装済みコマンド約20個
  （`net` `nodes` `region` `world`/`map` `moe` `infer` `dtr` `kdds` `pfs …` `hrw`
  `rgnpub` `kdemo` `ai` `dist` `rx` `ver` など）。**未知の入力は `[echo]` で返すだけ**であり、
  ベアメタル x86 の `raft`/`evolve`/`sfs`/`exec` 等は UMP には**存在しない**。
- **x86_64-linux UMP は普通にビルド・起動する。** 過去のドキュメントが
  「in progress」と過小に書いていたのは誤りで、aarch64-linux と同一のコマンド集合を持つ。
  （`p-kernel/README.md` の記述が古い場合、本ファイルが正である。）

---

## 2. 設計済み・実装中（designed / in-flight — まだ「動く」とは言わない）

- **設計ドキュメント群** — 思想と設計は
  [アーキテクチャ地図](p-kernel/docs/architecture/README.md) に一望できる
  （survival-network / regions / reflex-deliberation / p-fs / decentralized-lookup /
  r3-model-widening）。各 doc は「正直な論点」節で未解決問題を自己申告している。
- **R3 model widening** — 635 パラメータの玩具網を実用サイズへ広げ、初めて
  「1台に収まらない＝分散が必然」になる。doc は着地済み、**実装は本ブランチ未着手**。
  学習側（R3a）は並行ブランチで実装中（下記 §4）。
- **p-fs P3–P4** — 分散ルックアップとの統合・消失訂正符号。設計のみ。
- **lookup L2+** — world-table キャッシュ統合以降。設計のみ。

---

## 3. 構想（vision — 方向であって、現在ではない）

ここから先は**まだ存在しない**。だが、これがこのプロジェクトの魂であり、消さない。

- **宇宙船の装甲板1万枚**が各々 p-kernel を走らせ、装甲としての防御と計算基盤を兼ね、
  最後の1枚が生き残る限りネットワーク全体が死なない —— という生存ネットワーク。
- **考える器官** — 網全体で1つの脳。region が半球、反射と熟慮が別の時定数、
  p-fs が記憶。誰も所有しない AI の住処。
- **自己進化** — ノードが自分のコードを生成・コンパイル・配備して網ごと成長する
  （オンデバイス TCC＋`evolve` ループはこの方向の最初の配管デモであり、知能ではない）。

思想の全文は [survival-network.md](p-kernel/docs/architecture/survival-network.md) に逐語で置いてある。
これらを「動く機能」のように書いていた過去の文面は撤回する。

---

## 4. 率直な現状（honest caveats）

1. **AI は未学習である。** `infer` が動かす Transformer は 635 パラメータの玩具で、
   重みは LCG 疑似乱数で初期化されたまま（`arch/common/dtr.c` の `init_weights`）、
   **本ブランチ時点で一度も学習されていない**。つまり `infer` の出力は決定論的ノイズであり、
   意味のある分類ではない。いま実証できているのは「分散 forward-pass の配管が
   異種ABI・複数ノードを跨いで正しく流れること」であって、知能ではない。
   学習の実装（R3a）は並行ブランチ `r3a-train` で進行中だが、**本ブランチには未マージ**。
   重みが本当に学習されるまで、この網を「AI」と呼ぶ主張は配管の主張に格下げしておく。
2. **Federated Learning の損失関数はスタブである。** `arch/common/fedlearn.c` の loss は
   `(pred == label) ? 0.1f : 1.0f` であり、何も学習していない。FedAvg の通信経路の
   デモであって、学習のデモではない。
3. **ring-0 に浮動小数点 Transformer を置く矛盾**（PR #3 指摘）には、こう答える:
   ベアメタル側では FPU 状態管理が未解決の open item であり、推論コードのバグ1つで
   そのノードは落ちる。**今日の隔離の答えは Linux / Android ユーザモード移植（UMP）の側にある**
   — そこでは p-kernel 全体（AI を含む）が普通のユーザ空間プロセスとして走り、
   1ノードのクラッシュは群れの死を意味しない。**生存の単位は個体ではなく群れである。**
   ベアメタルはハードウェアまで降りるための研究の乗り物（research vehicle）であって、
   現時点の「死なない」ストーリーの担い手は UMP の群れの方である。
4. **テスト規律は relay に偏っている。** 自動テストらしい自動テストは relay の6シナリオと
   各 self-test（pfs / hrw / N=32 ハーネス）のみ。CI は未整備。
5. **リポジトリ衛生** — 入れ子の `p-kernel/p-kernel/` パス、コミット済みバイナリ等は
   既知の負債であり、別途整理中。

---

## 5. ライセンス（LICENSE）

ルートの `LICENSE` を参照（今回の整備の一環で追加）:
オリジナルコードは **BSD-3-Clause**、micro T-Kernel 由来コンポーネントは
**T-License 2.0** に従う。これまでバッジだけで本文が無かった状態を解消する。

---

> 各器官は別の生き物ではない。**同じ脳の、別の軸**である。
> —— 迷ったら [survival-network.md](p-kernel/docs/architecture/survival-network.md) へ戻る。
>
> *Built with love for a future where AI belongs to everyone — and documented
> honestly enough that you can check every claim yourself.*
