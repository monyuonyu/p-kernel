# INDEX — 全アーキテクチャドキュメント索引（complete, one row each）

> **これは `docs/architecture/**/*.md` の完全な索引である。** [[README.md]] §2 は
> 中核 9 本を 7 軸で描いた**概念地図**（読み筋）、[[V-MODEL.md]] は**設計↔検証の
> V スパイン**（各 doc の V-level と cert）、こちらは**全ドキュメントの一覧**（漏れ
> なく一度ずつ）である。迷ったら README §2 → V-MODEL → ここ → 各ドキュメント。
>
> **doc は V-level サブディレクトリに置かれている**（`00-concept/` `10-requirements/`
> `20-architecture/` `30-module/` `40-verify/` `50-evolution/`；meta は root）。
> リンクは basename の `[[wiki]]` で張る（Obsidian 規約で basename 解決するため、
> 移設してもリンクは切れない）。
>
> **現状の真偽表ではない。** 各行の `tier` は「その器官が今どれだけ実体か」の粗い
> ラベル、`cert` は V-MODEL のラング（対応する CI/自己テスト、無ければ「— (open rung)」）。
> SHIPPED/OPEN の正本は [[gap-ledger.md]] 一枚（OPEN 0）。
>
> **列の意味:**
> - **V** = V-level（L0 なぜ / L1 要件 / L2 アーキ / L3 詳細設計 / Verify / Evo 進化 / Meta）。
> - **cert** = 対応する検証（CI ジョブ / `[tag]` 自己テスト / live ジョブ）。詳細は [[V-MODEL.md]]。
> - **tier**: **working** = 出荷済・CI で守られる（一部 slice 残は「(部分)」）／ **designing** =
>   設計・計画（コード無〜部分）／ **vision** = 思想・北極星（dream-tier）／ **meta** = 索引・台帳。
>
> **不変条件（ドリフト防止）:** `docs/architecture/**/*.md` の **すべて**が、この索引に
> **ちょうど一度**現れること。ドキュメントを足した/`archive/` へ移した/改名/移設したら、
> 同じコミットでこの表を更新する（archive 済みは索引に載せず [[archive/README.md]] に載る）。
> 最終更新: **2026-07-01（V-MODEL restructure）**。

---

## mind / model — 心・モデル（1ノード内の推論と、その育て方）

| doc | 一行の役割 | V | cert | tier |
|---|---|---|---|---|
| [[living-mind.md]] | 会話から随時学び、睡眠で固定化する誰のものでもない心（LM-1..11） | L3 | `[dmn-*]`/`[lang-*]`/`[self-*]`/`[wmerge-*]` | working |
| [[native-student.md]] | 赤子から育つ器に合った脳（Cradle baby, NS-1） | L2 | `shipped-llm-certs` (student) | working |
| [[r3-nontrivial-thought.md]] | 「思考が玩具」ギャップを閉じた in-context 実学習（R3） | L2 | `[r3-incontext-*]` | working |
| [[r3b-breathing-params.md]] | 呼吸するパラメータ＝expert 専門分化（R3b） | L3 | `18_breathing` | working |
| [[r3-model-widening.md]] | 網を太らせて分散を必然にする論拠（living-mind で回答済の 年輪） | L2 | — (年輪 rationale) | designing |
| [[special-structure-mind.md]] | fleet 規模・疎・跨ノードの統一された心（SS-1..6 live, SS-7 設計） | L3 | `[expert-growth-preserves]`（SS-7 open） | working |
| [[inference-engine.md]] | 自前 LLM 推論エンジン＝GGUF ローダ＋量子化 matmul（M1） | L2 | `shipped-llm-certs` | working |
| [[memory-thought.md]] | forward パスが p-fs を読む＝記憶→思考の配線（wave 8） | L2 | — (open rung) | working |
| [[student-blob-transport.md]] | 可変長 student blob を運ぶ transport 再設計（SS-3 live） | L3 | `run_ss3_blob.sh` | working |
| [[conversational-teaching.md]] | 生きた教師ノードが子をネット越しに教育（cradle live L1/L2/L3） | L3 | `[teach-arrival]`/`[teach-live]`/`[teach-consolidated]` | working |
| [[conversation.md]] | 語彙の檻＝スケールの壁を出る戦略分岐の探索 | L3 | — (strategy exploration) | designing |
| [[moe-distillation-survey.md]] | 既製 MoE を借りるか蒸留して自前を育てるかの調査 | L2 | — (survey) | designing |
| [[base-model-survey.md]] | どの OSS base を借りるか（分岐 A2 の調査／採られなかった 年輪） | L2 | — (survey) | designing |

## survival / reflex — 生存・反射（死んでも群れが生き続ける環）

| doc | 一行の役割 | V | cert | tier |
|---|---|---|---|---|
| [[survival-network.md]] | **なぜこう作るか**。宇宙生存ネットワーク構想＝MoE を生存器官に（思想の核・逐語保存） | L0 | — (whole right arm) | vision |
| [[survival-loop.md]] | 内受容・分散ゲート・世界地図・冬眠・死をひとつの環に（L0+L1 SHIPPED/CI, L2–L4 設計） | L2 | `[survival-l0]`/`[survival-l1]` | working |
| [[reflex-deliberation.md]] | 反射層（近・速）／熟慮層（遠・遅）の二層と時定数分離＝発振しない脳 | L2 | `[moe-twolayer]`/`[moe-osc]`; `twolayer-couple-live` | working |
| [[reflex-action.md]] | 推論結果（class）を実在する局所防御へ繋ぐ＝思考に手足（G38 二層結合） | L2 | `[g38-*]`/`[g33-controlled]` | working |
| [[closed-loop.md]] | 個別に緑な部品を一本の閉制御ループに（負帰還整定, wave-12） | L2 | `composite-loop` (`20_closed_loop`) | working |
| [[composite-scenarios.md]] | 生存ループを閉じる複合シナリオ（audit G19, wave-11） | Verify | `composite-loop` (`22_composite`) | working |
| [[death-piercing.md]] | 推論の途中で死んでも群れが答える（配線③, wave-8） | L3 | live `survival-loop` (`13_survival_loop`) | working |
| [[fault-recovery.md]] | タスク隔離＋p-fs 重みからの再スポーン（wave-7） | L3 | live `survival-loop`; KILL-CHURN | working |
| [[genome.md]] | 空の装甲板にフル細胞を発芽させる自己再生（survival §3, wave-9） | L3 | `14_genome` | working |
| [[interoception.md]] | ノードの「痛み」を一本の `S_n` バスに束ねる（Slice-1 SHIPPED, Slice-2 設計） | L3 | `[survival-l0]` STATE bus（Slice-2 open） | working |
| [[survival-g38-impl-plan.md]] | survival §7 / G38 分散ゲーティング配線の実装計画（cert-first） | L3 | `[g38-*]` (realized) | designing |
| [[survival-recip.md]] | survival §7 受援側 recip 互恵ゲート（gacc の対）— **DECLINED 2026-07-04**：純粋利他を選択（自衛せず）。設計は歴史地層として保存 | L3 | `[recip-*]` (未実装) | declined |

## net / distributed — ネット・分散（中央なしで「1つの脳」を跨ノードに）

| doc | 一行の役割 | V | cert | tier |
|---|---|---|---|---|
| [[regions.md]] | 遅延でクラスタ化した region・locality-MoE・台数で増える容量 `capacity(N)`（R0–R2） | L2 | `[capacity-score]`/`[g13-arrival]`/`[g23-ceiling]` | working |
| [[decentralized-lookup.md]] | **探索の正準**。中央索引なしで所在を引く HRW/rendezvous/min-id 補題（L0/L1 impl, L2/L3 設計） | L2 | `[hrw]`/`[hrw-l1]`（L2/L3 open） | working |
| [[federation.md]] | 32 の壁から数千ノードへ橋を架ける階層フェデレーション（R0 LIVE+cert, F1–F3 設計） | L2 | `[g23-ceiling]`（F1–F3 open） | working |
| [[dynamic-id.md]] | churn 耐性のある node-id（relay lease SHIPPED, 完全 P2P 設計） | L3 | `[swim-incarn]`（full-P2P open） | working |
| [[p2p-overlay.md]] | Skype 原型的な中央なしメッシュ＋supernode 転送（N-2 slices SHIPPED, NAT 設計） | L2 | `[mesh-discovery]` (live)（NAT open） | working |
| [[n1-lan-direct-plan.md]] | 同一 WiFi で relay なし自動メッシュ（LAN-direct transport 設計計画） | L3 | — (open rung) | designing |
| [[relay-ha.md]] | 複数 relay と中央なしフェイルオーバ | L3 | `relay-tests` (6 scenarios) | working |
| [[connect-anywhere.md]] | Thread N の接続性硬化（heartbeat＋TCP fallback＋UDP↔TCP 自動切替） | L2 | `connect-anywhere-certs` (heartbeat/autofallback) | working |
| [[supernode-autopromote.md]] | 実測能力ベースの supernode 自動昇格（N-2d） | L3 | `connect-anywhere-certs` (autopromote) | working |
| [[full-smp-plan.md]] | 心を割らずに T-Kernel を SMP 化（②.0–②.2 SHIPPED, ②.3 ロードマップ） | L2 | `[smp-autodetect]` (run_smp0.sh) | working |

## storage / fs — 記憶（死なないファイルシステム）

| doc | 一行の役割 | V | cert | tier |
|---|---|---|---|---|
| [[p-fs.md]] | 内容アドレスで gossip 複製される死なない FS（P0–P2 live, P3–P4 設計） | L2 | `[pfs]`/`[pfs-durswallow]`/`[pfs-dagrefs]` | working |
| [[survival-fs.md]] | ARK＝洪水を生き延びる local FS（p-fs の durable backend） | L3 | `ark-crash-fuzzer`; `23_durable` | working |
| [[persistence.md]] | 忘れない方舟＝durable 層（SLICE 0+1+2 SHIPPED） | L3 | `[persist-identity]`/`[persist-mind]`/`[persist-mind-stale]` | working |

## device — 端末（器の性能・GPU に合わせる）

| doc | 一行の役割 | V | cert | tier |
|---|---|---|---|---|
| [[device-capacity.md]] | 端末の性能で担当量を変調（cluster 正準；DEVFIT-1 mind-sizing SHIPPED §0.5, tier/連続変調は設計） | L3 | `[device-fit]`（tier open） | designing |
| [[gpu-compute.md]] | 端末 GPU で心の数理を回す（Vulkan matmul backend SHIP 済, 心への統合は設計） | L3 | — (open rung) | designing |
| [[gpu-3-wiring.md]] | Vulkan matmul を心に配線（監査 verdict: DEFER implementation） | L3 | — (DEFER) | designing |

## ark / ux — 方舟・UX（人の目に見える星と、自分の体の観測）

| doc | 一行の役割 | V | cert | tier |
|---|---|---|---|---|
| [[ark-profile.md]] | 人類の記憶＝自伝の human chapter（v1 live, on the phones） | L3 | `[ark-consent]`/`[ark-profile]`/`[i18n-manifesto]` | working |
| [[galaxy.md]] | per-node 銀河の観測窓（v1 live, on the phones） | L3 | `[galaxy-serve]`/`[galaxy-events]`/`[galaxy-teach]` | working |
| [[living-body-inspector.md]] | 星の器官を REAL vitals に配線（HONEST-GLOW, on the phones） | L3 | — (via galaxy certs; open rung) | working |
| [[webd-user-space.md]] | web サーバを substrate の外へ出す（Slice A partial） | L3 | — (open rung) | designing |
| [[self-access.md]] | ノードが自分の体に触れる（READ-ONLY first slice） | L3 | — (open rung) | designing |

## evolution / ring3 — 進化（自己改変の核を ring3/EL0 へ）

| doc | 一行の役割 | V | cert | tier |
|---|---|---|---|---|
| [[ring3-core.md]] | 自己改変 AI 核を ring3/EL0 へ移設（推論路 SHIPPED, 学習モジュールは設計） | Evo | `ring3-survival` (`[ring3-mind]`/`[fpu-ctx]`…)（training open） | working |
| [[selfc-ring3.md]] | 自己ビルドした unit を免疫境界の内へ（v1 germ SHIPPED） | Evo | `[selfc-isolated]`/`[selfc-rollback]`/`[selfc-lineage]` | working |
| [[self-compile.md]] | 自己コンパイル（selfc）first milestone（fork germ＋capability 境界 SHIPPED） | Evo | `[selfc-*]` | working |

## compat — 互換（出荷後に進化し続ける群れの分裂を防ぐ）

| doc | 一行の役割 | V | cert | tier |
|---|---|---|---|---|
| [[compatibility.md]] | 古いノードと新しいノードが分裂しない世代継承戦略（compat 層 SHIPPED） | L1 | `[migrate-forward]` | working |
| [[compat-migration-chain-plan.md]] | 版ごと migration chain＋signed-OTA ゲート（SHIPPED） | L1 | `[migrate-forward]` + `[sign-*]` | working |

## meta — 索引・台帳・レビュー（root; not a rung）

| doc | 一行の役割 | V | cert | tier |
|---|---|---|---|---|
| [[README.md]] | 概念地図（5レイヤー・7軸の読み筋・依存関係図） | Meta | — | meta |
| [[V-MODEL.md]] | 設計↔検証の V スパイン（level 定義＋対応表＋OUTSIDE THE V） | Meta | — | meta |
| [[INDEX.md]] | 本索引（全ドキュメント一覧・この行を含む） | Meta | — | meta |
| [[gap-ledger.md]] | OPEN gap の唯一の生きた台帳（正本・行が減るのが進捗） | Meta | — | meta |
| [[BACKLOG.md]] | 設計/決定済み・未着手の master ordered TODO＋波の年表 | Meta | — | meta |
| [[review-2026-06-20-harsh.md]] | 敵対的な全プロジェクトレビュー（歴史地層として逐語保存） | Meta | — | meta |
