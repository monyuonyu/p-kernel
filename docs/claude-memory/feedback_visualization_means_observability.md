---
name: feedback-visualization-means-observability
description: "For mk_pino \"可視化/目に見える\" means a built-in decentralized observability mechanism in the system, NOT Claude exporting image files."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 53149c59-d57f-4589-aa45-bb25220e2df2
---

p-kernel の文脈で mk_pino が「目に見える」「可視化する」と言うとき、それは
**Claude が PNG/GIF などの画像成果物を書き出すこと**を意味しない。
**システム自身に「状況を可視化する仕組み」が機能として備わっていること**を指す。
具体的には「宇宙全体に広がったネットワークの全体像(どこが発火/暗い、どこに何の
デバイス)を、どのノードからでも見られる分散の可観測性層」。[[project_survival_network]]
の全体状況マップがまさにこれ。

**Why:** 2026-06-06、§10 ステップA を実装中に Claude が「可視化付き」を
「シミュレーション結果を絵にして見せる」と誤読し、matplotlib 代替のゼロ依存 PNG/GIF
レンダラを作り込む方向に逸れた。mk_pino が2度修正: (1)「画像で見えるとかではなくて、
ネットワーク全体の状況を可視化する仕組みがちゃんとある、という意味」(2)「カーネルの中
だけでなく、宇宙全体に広がるネットワークの状況 ― どこが発火しているか、どこに何の
デバイスがいるか ― の全体像が見れること」。これは仕組み(observability)であって
プレゼン用の絵ではない。

**How to apply:** 「可視化/見える化/状況が見たい」と言われたら、まず
「システムに組み込む観測機構」を想定する(分散・中央なし・gossip 由来)。画像を出力
したくなったら一旦止めて、求められているのが *仕組み* か *絵* かを確認する。p-kernel
では基本いつも *仕組み*。デモ画像はあくまで二次的・要確認。
