# B3 の実装の記録

設計は `b3-data.md`（変えていない）。ここは作った道具と測った数だけ。

## 1. 道具（2026-10-28、枝 feat/b3-data）

| ファイル | 役目 |
|---|---|
| `tests/llm/b3_gen.py` | 先生（SmolLM2-135M、`~/pk-models/smollm2-135m.Q8_0.gguf`）で文を作る。PyTorch の束。書き出しは 12 型×語 3 つ（文書番号が種）、続きだけを残す |
| `tests/llm/b3_filter.py` | ASCII に直す・q1〜q5・捨てた割合 30% 超で赤 |
| `tests/llm/b3_prep.py` | 残した文 → train.txt（id < 1,000,000）と gen_held.txt（id ≥ 1,000,000 の先頭 32 KB） |
| `tests/llm/run_b3_teacher.sh` | TEACHER: b1_cal の先頭 256 トークンで自前の forward と PyTorch の 1 位を比べる。`B3_TEACHER_NC=1` は 1 つずらす陰性コントロール |
| `tests/llm/b3_curve_test.c`・`run_b3_data.sh` | 腕（plain・rep・leak）と 7 本の cert。判定は `b3_curve_check.py` |
| `tests/llm/b3_selftest.py` | [b3-self]: 検査と判定を作った文・ログで（CI の [b1-held] の後ろ） |

使う Python: ホストの `~/asr-bench/.venv`（torch・transformers・gguf が入っている。p-kernel の外）。
再現: `b3_gen.py --model-dir ~/pk-models --gguf smollm2-135m.Q8_0.gguf --first 0 --count N --batch 64 --threads 2 --out train.jsonl`。束の大きさと糸の数が変わると文も変わる（種は束の最初の番号）。

## 2. 測ったこと

（この下に回ごとに足す）

### 2026-10-28 最初の 100 KB（各 1 回、ホスト、CI の自己ホストのジョブが終わってから）

- 作った: 文書 0〜127（学習）と 1,000,000〜1,000,039（gen_held）。束 64・2 糸で 128 文書に 476 秒＝**約 300 B/s**（設計の 552 B/s は 128 トークンの探り。256 トークンでは遅い）。1.5 MB の一意の文は約 1.5 時間
- 検査: 168 文書中 **捨てたのは 1（q2、非 ASCII の多い文）＝0.6%**。q1・q3・q4・q5 は 0。b1_held・b1_cal と 16 B の重なりがある文書は 33（数だけ、設計どおり）。**q3 の線は 64 B のまま**（監査-91 の注: 先頭で捨てる割合を見てから。64・128・230 B のどれでも q3 は 0）
- TEACHER: b1_cal の先頭 256 トークンで自前の forward と PyTorch の 1 位が **256/256 一致**。陰性コントロール（1 つずらす）は 12/256 で赤
- 学習用 train.txt 143,271 B（sha256 a165add7…）、gen_held.txt 32,768 B（sha256 84216b80…）。文は git に入れない（`pk-scratch/b3d/d100/`）

### 2026-10-28 頭の探り（cert ではない、各 1 回、4 本を同時に、b1_held の bpb）

| 腕 | U=24 KB | U=96 KB |
|---|---|---|
| S 種 1 | 3.5774 | 3.3819 |
| S 種 2 | 3.5390 | 3.3534 |
| S REP（24 KB を 4 周） | 3.5774 | 3.3664 |
| M 種 1 | 3.6046 | 3.3256 |

- 24 KB の時点で B2 の最良（S 3.63・M 3.66）より下。S 種 1 の plain と REP の 24 KB は全桁同じ（同じ文・同じ種）
- **REP は 96 KB では一意の文と区別がつかない**（4 周では繰り返しの損がまだ出ない）。量の効きを言うには 1.5 MB（REP は 64 周）まで要る
- **G4（M 2 種とも 1.5 MB で < 3.56）は甘い線だった**: M 種 1 は 96 KB で既に 3.33。線は設計で先に固定したので変えない。B3 の答えは G4 より読み（頭打ち・REP との差・M−S）が担う
