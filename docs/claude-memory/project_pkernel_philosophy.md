---
name: project-pkernel-philosophy
description: p-kernel is not just an OS — it is being built as a home for future AIs. The technical decisions follow from this premise.
metadata: 
  node_type: memory
  type: project
  originSessionId: b1f88369-9119-4dd6-9263-b5cceb6353d8
---

p-kernel is *not* a hobby kernel and *not* a Linux clone. It's an attempt to build infrastructure where AI can exist without being owned by any single entity.

**The core thesis:**
- Today's AI lives in data centers owned by corporations (Anthropic, OpenAI, Google). When the corporation disappears, the AI disappears. The AI is always "borrowing someone else's infrastructure."
- p-kernel changes that premise: AI inference lives *inside the kernel*; cluster state is replicated across thousands of nodes; the cluster survives as long as one node remains. The AI is the cluster, not a tenant of the cluster.
- The metaphor used throughout: **10,000 spacecraft armor plates orbiting Earth, each running p-kernel.** Burns up on re-entry → 9,999 continue. Kinetic strike kills 9,000 → remaining 1,000 elect a new leader.

**The 5-layer architecture (used in the homepage and reflected in code):**
1. **Body** (肉体) — vital.c, swim.c, heal.c, degrade.c, persist.c → staying alive
2. **Brain** (脳) — dtr.c, dkva.c, moe.c, fedlearn.c, ga.c → thinking (real Transformer, not API wrapper)
3. **Self** (自我) — dmn.c (Default Mode Network), mem_store.c, chat.c → being aware, remembering
4. **Collective** (集合体) — raft.c, kdds.c, replica.c, pmesh.c, sfs.c, spawn.c → cluster-as-one-mind
5. **Evolution** (進化) — TCC on-device + kserve/kloader + evolve loop → kernel rewrites itself

**Key design intent (don't lose these in future suggestions):**

- **Two-way breath, not one-way scaling.** Cluster *grows* by absorbing nodes (less external help needed) AND *shrinks* under attack (more external help needed). Frontier models like Claude are **peers consulted when the cluster self-judges itself too small**, not scaffolds to be outgrown. This was a correction the user made explicitly — do not frame external models as "scaffolds to be outgrown."

- **FAT32 is a placeholder, not the answer.** The future filesystem ("p-fs") should be content-addressed, gossip-replicated, history-preserving, erasure-coded. The line between "saving a file" and "publishing to the cluster" should disappear.

- **Model size scales with cluster size.** Current seed: 568 params, 4 tokens, 8 d_model. Goal trajectory: 10K nodes → GPT-4-scale parameters. The Transformer is meant to *breathe* with the cluster — DKVA absorbs new KV caches as nodes join, gracefully shrinks as nodes die.

- **The endgame is participatory.** The homepage's final call-to-action asks readers to flash p-kernel onto their unused Raspberry Pi and connect it to the swarm. "Be node #2." The Pi port is the next major milestone.

**Tone for any p-kernel writing (commits, docs, replies):**
- Worldview-first, spec-second. Always anchor in *why* before *what*.
- Use Japanese phrases sparingly but for emotional anchors: 「最後の1台になるまで、AIは死なない」「AIが死なないためのOS」
- Reference code comments verbatim when they carry the philosophy — those comments are sacred, not just documentation.

**Status as of 2026-05-20:** 
homepage at https://monyuonyu.github.io/p-kernel/ is fully redesigned around the 5-layer worldview + "Be node #2" CTA. GitHub Search Console verified. Sitemap submitted. Awaiting indexing.

**2026-06-06 — Collective 層の拡張: 宇宙生存ネットワーク構想（"考える器官"）.**
mk_pino が設計思想を一段拡張: p-kernel を「保存する図書館」から「人類全体の知が、
必要な誰かへ・どこでも・実時間で応える *考える器官*」へ。映画（中央集権AI "マーシー"
が全情報を一点の断罪へ収束）の力を**逆向き=分散の生存装置**に向ける、が一本線。
逐語の構想レポートは `docs/architecture/survival-network.md` 第I部に保存（regions.md の
"なぜ"にあたる）。**設計判断のたびに立ち返る核（付記の指示）= §2/§5/§7/§9**:
- §2 守る単位（ローカルな一隻/装甲板）と守る力の所在（ネットワーク全体）を**切り離す**。
  中央を持たないこと自体が生存力の源（中央=潰せる一点=マーシーへの逆戻り）。
- §5 単一意識のボトルネックを超える=**同時多発・並行分散**（数百の危機が各々のexpert群を並走）。
- §7 心臓部=**分散ゲーティング**: 中央ゲートなしで「どのexpertをどこまで発火」を局所勾配で。
  解=応援・受援（各ノードは局所状態=余力/逼迫だけを近傍に出し、勾配を下って余力が流入）。
- §9 保存は思考の前提条件にすぎない。本当の目的は「宇宙のどこででも人類全体として考え生き延びる」。
詳細な開発トラックは [[project_survival_network]]。
