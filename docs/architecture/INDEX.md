# INDEX — 全アーキテクチャドキュメント索引（complete, one row each）

> **これは `docs/architecture/*.md` の完全な索引である。** [[README.md]] §2 は
> 中核 9 本を 7 軸で描いた**概念地図**（読み筋）であり、こちらは**全ドキュメントの
> 一覧**（漏れなく一度ずつ）である。迷ったら README §2 → ここ → 各ドキュメント。
>
> **現状の真偽表ではない。** 各行の `tier` は「その器官が今どれだけ実体か」の粗い
> ラベルであって、SHIPPED/OPEN の正本は [[gap-ledger.md]] 一枚（OPEN 0）。
>
> **tier の意味:**
> - **working** = 出荷済み・CI で守られている（一部 slice が残るものは「(部分)」）。
> - **designing** = 設計/計画。コードは無い〜部分的。
> - **vision** = 思想・北極星（dream-tier）。
> - **meta** = 索引・台帳・レビュー（この索引自身を含む）。
>
> **不変条件（ドリフト防止）:** `docs/architecture/*.md` の **すべて**が、この索引に
> **ちょうど一度**現れること。ドキュメントを足した/`archive/` へ移した/改名したら、
> 同じコミットでこの表を更新する（archive 済みは索引に載せず
> [[archive/README.md]] に載る）。最終更新: **2026-07-01（doc-hygiene wave 2）**。

---

## mind / model — 心・モデル（1ノード内の推論と、その育て方）

| doc | 一行の役割 | tier |
|---|---|---|
| [[living-mind.md]] | 会話から随時学び、睡眠で固定化する誰のものでもない心（LM-1..11） | working |
| [[native-student.md]] | 赤子から育つ器に合った脳（Cradle baby, NS-1） | working |
| [[r3-nontrivial-thought.md]] | 「思考が玩具」ギャップを閉じた in-context 実学習（R3） | working |
| [[r3b-breathing-params.md]] | 呼吸するパラメータ＝expert 専門分化（R3b） | working |
| [[r3-model-widening.md]] | 網を太らせて分散を必然にする論拠（living-mind で回答済の 年輪） | designing |
| [[special-structure-mind.md]] | fleet 規模・疎・跨ノードの統一された心（SS-1..6 live, SS-7 設計） | working |
| [[inference-engine.md]] | 自前 LLM 推論エンジン＝GGUF ローダ＋量子化 matmul（M1） | working |
| [[memory-thought.md]] | forward パスが p-fs を読む＝記憶→思考の配線（wave 8） | working |
| [[student-blob-transport.md]] | 可変長 student blob を運ぶ transport 再設計（SS-3 live） | working |
| [[conversational-teaching.md]] | 生きた教師ノードが子をネット越しに教育（cradle live L1/L2/L3） | working |
| [[conversation.md]] | 語彙の檻＝スケールの壁を出る戦略分岐の探索 | designing |
| [[moe-distillation-survey.md]] | 既製 MoE を借りるか蒸留して自前を育てるかの調査 | designing |
| [[base-model-survey.md]] | どの OSS base を借りるか（分岐 A2 の調査／採られなかった 年輪） | designing |

## survival / reflex — 生存・反射（死んでも群れが生き続ける環）

| doc | 一行の役割 | tier |
|---|---|---|
| [[survival-network.md]] | **なぜこう作るか**。宇宙生存ネットワーク構想＝MoE を生存器官に（思想の核・逐語保存） | vision |
| [[survival-loop.md]] | 内受容・分散ゲート・世界地図・冬眠・死をひとつの環に（L0+L1 SHIPPED/CI, L2–L4 設計） | working |
| [[reflex-deliberation.md]] | 反射層（近・速）／熟慮層（遠・遅）の二層と時定数分離＝発振しない脳 | working |
| [[reflex-action.md]] | 推論結果（class）を実在する局所防御へ繋ぐ＝思考に手足（G38 二層結合） | working |
| [[closed-loop.md]] | 個別に緑な部品を一本の閉制御ループに（負帰還整定, wave-12） | working |
| [[composite-scenarios.md]] | 生存ループを閉じる複合シナリオ（audit G19, wave-11） | working |
| [[death-piercing.md]] | 推論の途中で死んでも群れが答える（配線③, wave-8） | working |
| [[fault-recovery.md]] | タスク隔離＋p-fs 重みからの再スポーン（wave-7） | working |
| [[genome.md]] | 空の装甲板にフル細胞を発芽させる自己再生（survival §3, wave-9） | working |
| [[interoception.md]] | ノードの「痛み」を一本の `S_n` バスに束ねる（Slice-1 SHIPPED, Slice-2 設計） | working |
| [[survival-g38-impl-plan.md]] | survival §7 / G38 分散ゲーティング配線の実装計画（cert-first） | designing |

## net / distributed — ネット・分散（中央なしで「1つの脳」を跨ノードに）

| doc | 一行の役割 | tier |
|---|---|---|
| [[regions.md]] | 遅延でクラスタ化した region・locality-MoE・台数で増える容量 `capacity(N)`（R0–R2） | working |
| [[decentralized-lookup.md]] | **探索の正準**。中央索引なしで所在を引く HRW/rendezvous/min-id 補題（L0/L1 impl, L2/L3 設計） | working |
| [[federation.md]] | 32 の壁から数千ノードへ橋を架ける階層フェデレーション（R0 LIVE+cert, F1–F3 設計） | working |
| [[dynamic-id.md]] | churn 耐性のある node-id（relay lease SHIPPED, 完全 P2P 設計） | working |
| [[p2p-overlay.md]] | Skype 原型的な中央なしメッシュ＋supernode 転送（N-2 slices SHIPPED, NAT 設計） | working |
| [[n1-lan-direct-plan.md]] | 同一 WiFi で relay なし自動メッシュ（LAN-direct transport 設計計画） | designing |
| [[relay-ha.md]] | 複数 relay と中央なしフェイルオーバ | working |
| [[connect-anywhere.md]] | Thread N の接続性硬化（heartbeat＋TCP fallback＋UDP↔TCP 自動切替） | working |
| [[supernode-autopromote.md]] | 実測能力ベースの supernode 自動昇格（N-2d） | working |
| [[full-smp-plan.md]] | 心を割らずに T-Kernel を SMP 化（②.0–②.2 SHIPPED, ②.3 ロードマップ） | working |

## storage / fs — 記憶（死なないファイルシステム）

| doc | 一行の役割 | tier |
|---|---|---|
| [[p-fs.md]] | 内容アドレスで gossip 複製される死なない FS（P0–P2 live, P3–P4 設計） | working |
| [[survival-fs.md]] | ARK＝洪水を生き延びる local FS（p-fs の durable backend） | working |
| [[persistence.md]] | 忘れない方舟＝durable 層（SLICE 0+1+2 SHIPPED） | working |

## device — 端末（器の性能・GPU に合わせる）

| doc | 一行の役割 | tier |
|---|---|---|
| [[device-capacity.md]] | 端末の性能で担当量を変調（cluster 正準；DEVFIT-1 mind-sizing SHIPPED §0.5, tier/連続変調は設計） | designing |
| [[gpu-compute.md]] | 端末 GPU で心の数理を回す（Vulkan matmul backend SHIP 済, 心への統合は設計） | designing |
| [[gpu-3-wiring.md]] | Vulkan matmul を心に配線（監査 verdict: DEFER implementation） | designing |

## ark / ux — 方舟・UX（人の目に見える星と、自分の体の観測）

| doc | 一行の役割 | tier |
|---|---|---|
| [[ark-profile.md]] | 人類の記憶＝自伝の human chapter（v1 live, on the phones） | working |
| [[galaxy.md]] | per-node 銀河の観測窓（v1 live, on the phones） | working |
| [[living-body-inspector.md]] | 星の器官を REAL vitals に配線（HONEST-GLOW, on the phones） | working |
| [[webd-user-space.md]] | web サーバを substrate の外へ出す（Slice A partial） | designing |
| [[self-access.md]] | ノードが自分の体に触れる（READ-ONLY first slice） | designing |

## evolution / ring3 — 進化（自己改変の核を ring3/EL0 へ）

| doc | 一行の役割 | tier |
|---|---|---|
| [[ring3-core.md]] | 自己改変 AI 核を ring3/EL0 へ移設（推論路 SHIPPED, 学習モジュールは設計） | working |
| [[selfc-ring3.md]] | 自己ビルドした unit を免疫境界の内へ（v1 germ SHIPPED） | working |
| [[self-compile.md]] | 自己コンパイル（selfc）first milestone（fork germ＋capability 境界 SHIPPED） | working |

## compat — 互換（出荷後に進化し続ける群れの分裂を防ぐ）

| doc | 一行の役割 | tier |
|---|---|---|
| [[compatibility.md]] | 古いノードと新しいノードが分裂しない世代継承戦略（compat 層 SHIPPED） | working |
| [[compat-migration-chain-plan.md]] | 版ごと migration chain＋signed-OTA ゲート（SHIPPED） | working |

## meta — 索引・台帳・レビュー

| doc | 一行の役割 | tier |
|---|---|---|
| [[README.md]] | 概念地図（5レイヤー・7軸の読み筋・依存関係図） | meta |
| [[INDEX.md]] | 本索引（全ドキュメント一覧・この行を含む） | meta |
| [[gap-ledger.md]] | OPEN gap の唯一の生きた台帳（正本・行が減るのが進捗） | meta |
| [[BACKLOG.md]] | 設計/決定済み・未着手の master ordered TODO＋波の年表 | meta |
| [[review-2026-06-20-harsh.md]] | 敵対的な全プロジェクトレビュー（歴史地層として逐語保存） | meta |
