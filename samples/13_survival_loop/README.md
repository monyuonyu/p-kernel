# 13_survival_loop — 死を貫通する一周

「死なないための OS」を名乗りながら、推論の最中に一度もノードを殺して
確かめたことがなかった。このハーネスはそれを 3 回やる。

```
./kill_one.sh        # 失敗時 exit 非0。ログは /tmp/sl13_*.log
```

各シナリオは relay (`:7413`) + 3 ノードのクラスタを毎回新規に起動し、
`dkva infer 50 20 90 5` (分散 KV attention、引数から決定論的に Q を合成
するのでどのノードから発行しても「同じ問い」) を駆動する。

| シナリオ | 殺すもの | アサート |
|---|---|---|
| A | 非起点 responder を SIGKILL → 直後に推論 | `degraded (2/3)` を明示して完遂。E_TMOUT ゼロ |
| B | **起点**を SIGKILL | 生存ノードが同じ問いを発行して完遂 (起点に特権はない) |
| C | 連続推論ストリームの最中に responder を SIGKILL | 10 回の推論が全部完遂。SWIM 収束中は `degraded (2/3)` が正直に出る |

## 部分集約の正直さ規約

`dkva_infer` (arch/common/dkva.c) は fan-out 時に SWIM が生存と見ていた
peer の集合を「期待」として記録する:

- 期待 peer が欠けたまま完遂したら、必ず
  `[dkva] degraded (k/n): completed with partial aggregation` を出す
  (k/n = 自分込みの寄与ノード数 / 期待ノード数)。黙って成功にしない。
- SWIM が DEAD と判定済みのノードは最初から待たない。待っている間に
  DEAD になったら期待から外す (`not waiting for node N (SWIM: DEAD)`)。
- リモート寄与がゼロのときだけ E_TMOUT (従来どおり)。

## シナリオ A の kill タイミングについて

仕様上は「fan-out 直後に SIGKILL」だが、bash から fan-out と最初の応答
(数十 ms) の間に決定論的にシグナルを差し込むことはできない。そこで
SIGKILL を fan-out の**直前**に置く: SWIM の stale-ALIVE 窓 (probe 周期
1s × SUSPECT 2 ラウンド) の内側なので、起点は死んだノードを「生きている」
と思って fan-out し、応答が来ないまま部分集約で完遂する — 検証される
コード経路は「fan-out 直後に死んだ」場合と同一。

## 残課題 (docs/architecture/death-piercing.md)

起点が死んだ場合、その推論自体は失われる (シナリオ B は「誰でも問い
直せる」ことの証明であって、自動引き継ぎではない)。問いの永続化と
自動再発行は次の波。
