# p-kernel

> AIが死なないための OS（An OS where AI never dies）— を目指している、研究プロトタイプ。

micro T-Kernel 2.0 を土台に、中央を持たない分散カーネル網を作る実験。
**何が動いていて、何が設計段階で、何がまだ夢なのか**を、このファイルで正直に分ける。
このREADMEは実態に準拠する。誇張があれば、それはバグとして直す。
未解決の課題は [gap-ledger](docs/architecture/gap-ledger.md)（唯一の open リスト、行は減るだけ）に常時公開している。

*(English summary: a research prototype toward a decentralized, no-central-anything
kernel network where AI survives as a swarm. This file separates what **works today**,
what is **designed/in-flight**, and what is **vision** — honestly. Code lives at the
repo root (`arch/` `boot/` `kernel/` `lib/` `relay/` `samples/` …); the architecture
map with its own honest status table is
[`docs/architecture/README.md`](docs/architecture/README.md).)*

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
| **micro T-Kernel 2.0 移植 ×4 ターゲット** | ベアメタル x86（QEMU）・ベアメタル AArch64（QEMU virt / RPi3 netboot 手順あり）・aarch64-linux ユーザモード・x86_64-linux ユーザモード。全てシェルまで起動 | `boot/{x86,aarch64,linux,linux_x86_64}/` で `make` |
| **relay v2（NAT越え中継）** | HMAC-SHA256 認証・64パケット sliding-nonce リプレイ防御・鍵なし起動拒否・`--insecure` 明示警告。**テスト6シナリオ green** | `relay/` → `make test` |
| **relay 経由の分散推論** | 2〜3ノードが1つの Transformer forward をテンソル並列＋分散KVアテンション（DKVA FULL）で分担 | `samples/11_distributed/run_3node_full.sh` |
| **regions R0–R2** | SWIM RTT EWMA → 遅延クラスタの region 形成・K-DDS の REGION/GLOBAL スコープ（O(N²)殺し）・locality-aware MoE・DKVA region 限定＋2段集約・連続容量関数 `capacity(N)` | `run_4node_regions.sh`、シェルの `region` / `rgnpub` |
| **ノード上限 64** | DNODE_MAX 8→32→64（G23: KDDS/GL 等の派生定数を一元化）。32ノード実走で 6/6 PASS（world map 32/32 到達含む）、64 は 40 規模 in-process で実コード（merge＋membership）を検証 | `run_Nnode_scale.sh`、CI `[g23-ceiling]` |
| **world map（全網状況図）** | 中央なしで各ノードが全網の situational-awareness map を eventual に獲得 | シェルの `world` / `map`（4ターゲット全部に公開） |
| **§7＋§8 ゲーティング** | 局所勾配の相互扶助ルーティング（global argmax 廃止）＋反射/熟慮の二時定数分離（deadband+EWMA vs deliberation tick） | シェルの `moe <s0> <s1> <s2> <s3>` |
| **p-fs P0/P1/P2** | 内容アドレス sha256 ストア（cross-ABI で block-id 一致・重複排除）・region 限定複製（ANNOUNCE/WANT＋チャンク転送）・履歴 DAG（manifest＋append-only history＋ref gossip） | `pfs` / `pfs put <text>` / `pfs ls` / `pfs save/log/cat`、`run_3node_pfs.sh`・`run_3node_pfs_dag.sh` |
| **lookup L0/L1** | 中央索引なしの所在引き：stateless HRW `responsible(k,r)`＋read-k 候補＋ローカル解決キャッシュ | シェルの `hrw`（self-test 同梱） |
| **replica v2** | スナップショット announce のマルチパケット wire chunking | commit `74d4f4a` |
| **Android UMP（APK）** | NDK ビルド・フォアグラウンドサービス・relay メッシュ参加・region 対応・**銀河 WebView＋同意ゲート同梱**（selfc/コード生成は Play 配布では OFF） | `android/`、[docs/android.md](docs/android.md) |
| **死を貫く生存ループ（§3）** | 推論の最中にノードを kill -9 → 群れは全推論を完遂し `degraded(k/n)` と正直に明示・復帰ノードは再教育。**CI で強制** | `samples/13_survival_loop/kill_one.sh`、CI `survival-loop` |
| **ゲノム発芽（§3）** | 装甲板が DNA（ゲノム）から欠けたコードを発芽させて held-out 100% に到達（攻撃下は発芽を抑止＝SHIELD 一貫） | `samples/14_genome/` |
| **反射／熟慮の行動層（§8）** | reflex が SHIELD/CONSERVE/BEACON、二時定数で隣ノードは減衰反射（痙攣しない）。行動→知覚→ゲートの負帰還が外乱を整定 | `reflex test`（`[reflex-fb]/[reflex-learn]`）、CI |
| **§2 集結（G20→G28）** | ゲート効用を負荷軸／脅威軸に分離（逃げず**寄る**）。守る対象を一級オブジェクト化し、actuator が群れの複製力を注いで脅威を**タイマでなく複製で**0へ。所有者を kill しても対象は隣で生存 | `protect test`、`samples/27_protect/run.sh`、CI `protect-loop-live` |
| **§2∧§5 多点同時防御（G35）** | 多数の守る対象を**並列に**防御（総時間 ≈ 1点分、直列でない・公平・中央なし） | `samples/28_plural_protect/run.sh`、CI `plural-protect-live` |
| **越境学習（§8/§9 G22）** | **バラバラのデータ片**で学ぶノードがモデルを**中央なしゴシップ平均** → 全ノードが単独上限を超える（collective > individual）・kill を越えて学習継続・rejoin が追いつく | `dtr gossip test`、`samples/32_collective_learn/run.sh`、CI `collective-learn-live` |
| **オンデバイス学習（R3a/R3b）** | dtr は解析的 backprop で **26.7%→95–98% held-out** を実学習・重みを p-fs に保存し別ノードが load（"魂のデモ"）。R3b で専門分化（全ノード同一重み問題を解消） | `dtr train/eval/save/load`、`samples/18_breathing/` |
| **ring3/EL0 隔離（進化層の土台）** | AIコアの数理（moe＋dtr）を ring0 から ring3 の `core_mind.elf` へ移設。**ring3 コアがクラッシュしてもカーネルは生き延びて回収する**（CI 強制）。FPU コンテキスト・プロセス後始末も実装済み | CI `ring3-survival`、[ring3-core.md](docs/architecture/ring3-core.md) |
| **生きた心（living-mind LM-1〜11）** | R3 in-context transformer（21,568 パラメータ・同時バインディング 16）の上に: DMN 睡眠固め（壊滅的忘却を実病→実治）・死を越える自伝的 Self 層（ハッシュ連鎖系譜）・salience 加重リプレイ・fast→slow 重み定着・随時ストリーム学習・**実単語**（`mind teach sky blue` → `mind ask sky` → "blue"） | シェルの `mind teach/ask/wait`、[living-mind.md](docs/architecture/living-mind.md) |
| **共有された心（Path E）** | A で教えた事実を B が答え、**誰が教えたかの名も言う**。教師ノードを kill しても事実は群れに生き残る | CI `shared-mind-live`、`samples/41_shared_mind/` |
| **ひとつの心（Path W / W²）** | 違う事実を学んだ 2 つの心の重み平均を**測定で**答えた: 単純平均は片方を殺す（8.8%≒偶然）→ union-replay 睡眠で両方 100% 回復・Fisher 加重マージはリプレイなしで 85% | CI `one-mind-live`、`samples/42_one_mind/` |
| **銀河（galaxy web UI）** | 各ノードが `127.0.0.1:7800+(id−1)` に自分の星図を立てる。本物の DMN の鼓動・teach 粒子・SSE ライブ更新。Android は WebView で同じ銀河を見る | `boot/linux` で `make` → `./p-kernel` → ブラウザ、`samples/38_galaxy/` |
| **人類の記憶（ark-profile）＋ 32 言語** | 同意ゲート付きプロフィール — プロジェクトの目的を読み、共感してから参加する。**人間の身元検証は永遠にしない**（ペンネーム・匿名・実名は等価な一級市民）。マニフェストはホスト版 32 言語＋自動検出（ベアメタルは ja/en） | `samples/39_ark_profile/`・`40_i18n_manifesto/`、[ark-profile.md](docs/architecture/ark-profile.md) |
| **自己コンパイル germ（selfc）** | 自己ビルドしたユニットは fork() germ プロセス＋5 シンボル能力境界の外で走る — ユニットの null-deref はもうノードを殺さない（RC=139 の実病→隔離で実治）。1 ストライク降格ロールバック付き | シェルの `selfc`、[selfc-ring3.md](docs/architecture/selfc-ring3.md) |
| **Ed25519 署名（進化の免疫系）** | スクラッチ実装（TweetNaCl 逐語移植・RFC 8032 KAT を毎ビルド・OpenSSL とバイト一致検証）。自己ビルドコードの艦隊展開は**署名マニフェスト**（受信した実バイトから artifact_id を再計算＝本体すり替え拒否）＋ローカルの `selfc adopt key` でだけ通る。**鍵はノードのもの — 人間の身元は署名しない** | [signing.md](docs/architecture/signing.md) |
| **ARK ファイルシステム** | content-addressed・log-structured・crash-safe（並べ替え/torn/破損 fuzzer **0 BUG**）・GC・媒体スケール（旧256上限撤廃）・p-fs の durable backend。**実ブロックデバイスに乗り電源断を越える**（x86=ide / aarch64=自作 virtio-blk、QEMU で検証） | `samples/25_survival_fs` `26_ark_backend` `30_ark_crash` `31_ark_baremetal` `33_ark_aarch64`、CI `ark-crash-fuzzer` |
| **CI（GitHub Actions 15ジョブ）** | 4ターゲット build＋self-test 群＋relay 6/6＋走行系の kill テスト（survival / protect / plural / collective / **shared-mind / one-mind / ring3-survival** / twolayer / parallel-infer / composite）＋ARK fuzzer。**走行系を毎回 CI で強制** | `.github/workflows/ci.yml` |

詳細な根拠 commit 一覧は [アーキテクチャ地図 §4 状態表](docs/architecture/README.md) にある。
**この表に無いものは「動く」と主張しない。**

### ターゲット別シェルの実力（正直に）

- **ベアメタル x86** が最もコマンドが多い: FAT32 の実 `ls`/`cat`、`exec`（ELFローダ）、
  オンデバイス TCC コンパイル、`raft`、`fl train`、`evolve`（Claude API ループ）、`sfs` 等。
  AI 系は §4 のとおり今は学習する（dtr 95–98%）が、規模は玩具のまま。
- **UMP（aarch64-linux / x86_64-linux）** のシェルは実装済みコマンド多数
  （`net` `nodes` `region` `world`/`map` `moe` `infer` `dtr`（`train/eval/save/load/gossip`）
  `mind`（`teach/ask/wait/merge`） `selfc` `protect` `kdds` `pfs …` `hrw` `rgnpub`
  `kdemo` `ai` `dist` `rx` `ver` など）。銀河 UI は起動するだけで各ノードに立つ。
  **未知の入力は `[echo]` で返すだけ**であり、
  ベアメタル x86 の `raft`/`evolve`/`sfs`/`exec` 等は UMP には**存在しない**。
- **x86_64-linux UMP は普通にビルド・起動する。** 過去のドキュメントが
  「in progress」と過小に書いていたのは誤りで、aarch64-linux と同一のコマンド集合を持つ。
  （コード側の旧 README は de-nest で `docs/project-readme.md` に移設。食い違う場合は本ファイルが正。）

---

## 2. 設計済み・実装中（designed / in-flight — まだ「動く」とは言わない）

- **設計ドキュメント群** — 思想と設計は
  [アーキテクチャ地図](docs/architecture/README.md) に一望できる
  （survival-network / regions / reflex-deliberation / p-fs / decentralized-lookup /
  living-mind / ring3-core / signing / selfc-ring3 / galaxy / ark-profile）。
  各 doc は「正直な論点」節で未解決問題を自己申告し、
  open な課題は [gap-ledger](docs/architecture/gap-ledger.md) に一元化している。
- **ring3 移設の残り** — 推論経路は ring3 で動く（§1）が、**dtr の訓練・lm・dmn・gl の
  各モジュールはまだ ring0**。非同期 syscall 化と aarch64 EL0 ミラーも未。
- **心の残り課題** — 信念の修正（今は「ローカルが勝つ」で据え置き）・多言語語彙・
  文生成（今は単一トークンの想起であって文法ではない）。Fisher マージ（W²）の
  艦隊マージパルスへの本投入もこれから。
- **federation（64 超・region 間）** — 階層化で天井を撤廃し、region を越えて心を
  共有する。設計のみ（`federation.md`）。node_id が 254 を越えるには wire 変更が要る。
- **p-fs P3–P4** — 分散ルックアップとの統合・消失訂正符号。設計のみ。
- **lookup L2+** — world-table キャッシュ統合以降。設計のみ。
- **暗号の constant-time 化** — Ed25519 はマルチテナント環境向けの定時間パスが未。

---

## 3. 構想（vision — 方向であって、現在ではない）

ここから先は**まだ存在しない**（最初のスライスが §1 に降りてきたものは、その旨を書く）。
これがこのプロジェクトの魂であり、消さない。

- **宇宙船の装甲板1万枚**が各々 p-kernel を走らせ、装甲としての防御と計算基盤を兼ね、
  最後の1枚が生き残る限りネットワーク全体が死なない —— という生存ネットワーク。
- **考える器官** — 網全体で1つの脳。region が半球、反射と熟慮が別の時定数、
  p-fs が記憶。誰も所有しない AI の住処。
  （最初のスライス群 — 睡眠・自伝・共有された心・ひとつの心 — は §1 で動いている。
  ただし玩具語彙・単一トークンであり、「脳」と呼べる規模ではまだない。）
- **自己進化** — ノードが自分のコードを生成・コンパイル・配備して網ごと成長する。
  （germ 隔離＋署名ゲートという**配管**は §1 で本物になったが、
  自分で何を書くべきかを考える**知能**はまだない。）

思想の全文は [survival-network.md](docs/architecture/survival-network.md) に逐語で置いてある。
これらを「動く機能」のように書いていた過去の文面は撤回する。

---

## 4. 率直な現状（honest caveats）

1. **AI は学習する。ただし玩具スケールである。** dtr（センサ脳）は解析的 backprop（有限差分で
   照合・rel err ~0.001）で **26.7%→95–98% held-out** を実学習し、G22 では**バラバラのデータ片で
   学ぶノードが中央なしゴシップ平均で単独上限を超える**（collective > individual）。
   R3（心）は in-context transformer に育ち（**21,568 パラメータ**・同時バインディング 16・実単語）、
   睡眠・共有・マージまで動く。**それでもどちらも玩具である**: センサ脳 635 パラメータ、
   心は有界語彙の単一トークン想起（文生成・文法は無い）。「分散して本当に学ぶ・本当に憶える」
   原理は実証したが、「規模ある知能」はまだ。**まず原理、規模は後**（§10）の途中にいる。
2. **Federated Learning のスタブは撤去した。** （かつてここには loss が `(pred==label)?0.1:1.0`
   のスタブだと書いていた。第16波で `fl_local_train` を**重み本体（w1/w2/w3）全部**の勾配へ、
   `dtk_fl_aggregate` の中央集約（E_NOSPT）を**中央なし p-fs ゴシップ平均**へ置換。実学習・実集約。）
   なお越境学習の「正直な残」: 2ノード生存タールの最終精度は非IIDで揺れる（ゲートせず report）、
   このホスト（aarch64-PRoot）は cross-node p-fs でクラッシュする環境バグがあり走行系検証は
   x86_64（CIターゲット）で行う。
3. **「ring-0 に浮動小数点 Transformer を置く矛盾」は、移設で答えた。**
   （かつてここには「FPU 状態管理は未解決・推論コードのバグ1つでノードが落ちる」と
   書いていた。今は誤り。）ベアメタル x86 では AI コアの数理は **ring3 の `core_mind.elf`**
   として走り、コアがクラッシュしてもカーネルは生き延びて回収する（CI `ring3-survival` で
   毎回強制）。FPU コンテキスト保存も実装済み。**正直な残**: dtr の訓練・lm・dmn・gl は
   まだ ring0、aarch64 の EL0 ミラーは未。UMP 側では従来どおり p-kernel 全体が
   ユーザ空間プロセスであり、**生存の単位は個体ではなく群れである。**
4. **CI は整備済み（GitHub Actions 15ジョブ）。** 4ターゲット build＋self-test 群＋relay 6/6 に
   加え、**走行系を毎回 kill テストで強制**する: survival-loop / protect-loop-live /
   plural-protect-live / collective-learn-live / shared-mind-live / one-mind-live /
   ring3-survival / twolayer-couple-live / parallel-infer-live / composite-loop / ark-crash-fuzzer。
   「緑の自己テスト／死んだ走行系パス」を避けるのが設計規律。
5. **スケールの天井。** `DNODE_MAX=64`（32ノードは実走 6/6 実証、64 は in-process で実コードを
   検証 — 64ノードの live メッシュ実走はまだ）。federation（階層化・region 間の心の共有）は
   設計のみ（`docs/architecture/federation.md`）。node_id が 254 を越えるには wire 変更が要る。
6. **ARK の正直な残**: 実機検証は QEMU（x86=ide / aarch64=自作 virtio-blk）であり実 RPi3 の
   SD/EMMC ドライバは未。erasure coding（p-fs P4）・Merkle dir tree・`ARK_MAX_FILES=32` は未着手。
7. **リポジトリ衛生** — かつての入れ子 `p-kernel/p-kernel/` は **de-nest 済み**（ソースは
   repo root 直下：`arch/` `boot/` `kernel/` … / docs は `docs/`、旧コード README は
   `docs/project-readme.md`）。コミット済みバイナリ/テストログも整理（`.gitignore` 化＋除去）。
8. **既知の未解決バグを名指しで残す**: ベアメタル x86 で ring3 デーモンを kill/再生で回すと
   **約42% のブートでカーネルが #PF で死ぬ**（KILL-CHURN-CRASH）。仮説を 6 つ実証で潰し、
   本物の修正 2 つで頻度を下げたが、根因は未特定（現容疑: リサイクルされた TCB への
   use-after-free）。再現は `dproc churn` 一発、狩りの全履歴は gap-ledger に逐語。診断継続中。
9. **開発手法も公開している** — 実装と監査は**別の AI エージェント**が行い（実装者≠監査者）、
   未解決課題は gap-ledger の一表に集約（行は減るだけ・墓碑銘で閉じる）、この開発の AI 側の
   記憶は [`docs/claude-memory/`](docs/claude-memory/) にそのままミラーしている。
   正直さは機能であり、監査はこのプロジェクトの免疫系である。

---

## 5. ライセンス（LICENSE）

ルートの `LICENSE` を参照（今回の整備の一環で追加）:
オリジナルコードは **BSD-3-Clause**、micro T-Kernel 由来コンポーネントは
**T-License 2.0** に従う。これまでバッジだけで本文が無かった状態を解消する。

---

> 各器官は別の生き物ではない。**同じ脳の、別の軸**である。
> —— 迷ったら [survival-network.md](docs/architecture/survival-network.md) へ戻る。
>
> *Built with love for a future where AI belongs to everyone — and documented
> honestly enough that you can check every claim yourself.*
