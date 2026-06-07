# Latency benchmark — putting a number on §8 (the light-speed wall)

最終更新: 2026-06-07 ／ wave-15 B隊 ／ 俯瞰監査 **G31**(§4 の核 latency/light-speed が未計測)への回答
関連: [survival-network.md §4/§8/§10-B](../architecture/survival-network.md),
[reflex-deliberation.md](../architecture/reflex-deliberation.md)(時定数の分離),
[locality.md](locality.md)(§4 の前半 traffic/energy; その (D) latency 未測がここの出発点)
ハーネス: [`samples/29_latency/run.sh`](../../samples/29_latency/),
sim: [`tools/sim/latency_twolayer_sim.py`](../../tools/sim/latency_twolayer_sim.py)

## 0. なぜこの計測が要るのか

`locality.md` は §4 の前半(「遠方通信のバイト数/件数を減らす」)を数で示したが、
その **(D) で最大の正直な限界**を残した:

> **latency / 光速 — 測れていない＝未達。** RTT zone ペナルティは region 形成の
> ための観測 RTT を水増しするだけで、実際の per-packet 遅延を一切注入しない。
> §4 の核心である「光速の壁」への効果は **モデル上の概念であって本計測では
> 検証されていない**。

俯瞰監査 **G31** が同じ点を死角として挙げた。本ドキュメントはその穴を埋める ――
**実際に per-packet 遅延を注入し**、§8 の二層構造(反射層=近傍・即応 / 熟慮層=
全体・遅延)が、遅延の下で **時定数として分離していること**を REAL relay 上で実測する。
合否ではなく **計測**が目的。何がモデルで何が実測かを終始分ける。

## 1. 距離→遅延モデル(これがこの波で新しく作ったもの)

### 1.1 relay の遅延ノブ(実装)

`relay/relay.c` に **per-destination の転送遅延**を追加した。既定 OFF
(env 未設定 = 遅延 0 = ノブ導入前と byte-for-byte 同じ転送パス。relay 6/6 テストは不変)。

| env | 意味 |
|-----|------|
| `RELAY_DELAY_MS`     | 全宛先に一律に与える基本遅延(既定 0) |
| `RELAY_FAR_NODES`    | 「遠い」とみなす宛先 node id のカンマ列 |
| `RELAY_FAR_DELAY_MS` | その遠い宛先の遅延(near 宛は 0 のまま) |

**重要な設計判断**: 遅延は `usleep` でブロックしない。素朴に「far 宛の送信前に sleep」
すると、その間 relay 全体が止まり **near のパケットが far の後ろで head-of-line
ブロック**される ―― それでは「far が near の即応性を損なわない」という §8 の主旨を
自分で壊す。代わりに far 宛パケットは *deliver-at タイムスタンプ付きでキューに積み*、
`poll()` 駆動のメインループが期限の来たものだけを flush する。これにより
**near の転送は即時のまま、far だけが待つ**。ノブ OFF 時はキューが常に空で
`next_due = ∞ → poll(timeout=-1)` となり、従来の blocking recvfrom に縮退する。

### 1.2 光速 → 遅延の対応(sim 側・モデル)

`tools/sim/latency_twolayer_sim.py` が `delay = 距離 / c` を計算する(**モデル**):

| 距離 | 片道光速遅延 |
|------|--------------|
| near region (RTT≤τ=50ms) | 25 ms(距離でなく τ/2 で定義) |
| Moon (3.84×10⁵ km) | 1.28 s |
| Mars 最小 (5.46×10⁷ km) | 182 s(3.0 min) |
| Mars 平均 (2.25×10⁸ km) | 750 s(12.5 min) |
| Mars 最大 (4.01×10⁸ km) | 1338 s(22.3 min) |

時定数はカーネル値: 反射 `tau_r=200ms`、熟慮 `tau_d=2000ms`(比 10×、
[reflex-deliberation.md](../architecture/reflex-deliberation.md) D2 の実測比 fast=4/slow=40 と整合)、
gossip 5s、`REGION_TAU_MS=50`。

## 2. 計測(REAL relay 上の実測)

`samples/29_latency/run.sh`: REAL `./relay` を `RELAY_FAR_NODES=3
RELAY_FAR_DELAY_MS=300`(near=0)で起動。`latency_client` が 1 プロセスで 3
ソケット(ship=1 / near 反射 peer=2 / far 熟慮 peer=3)を握り、ship から
往復プローブを撃つ(カーネルと同じ v2 wire)。

**決定的テスト = 非干渉(non-interference)**: t=0 に far プローブを 1 発撃ち、
それが relay の遅延キューに居る *間に* near プローブを 20ms 間隔で連射する。
二層が分離していれば near は全て ~1ms で往復し far より先に返る。relay が near を
far の後ろでブロックすれば near は far_delay ぶん固まる。

### 2.1 結果(3 回実行、本ホスト localhost)

```
RESULT: PASS  near_max=1.0ms(<50ms reflex budget) far=301.0ms noninterference=yes separation=301x
RESULT: PASS  near_max=1.0ms(<50ms reflex budget) far=300.0ms noninterference=yes separation=300x
RESULT: PASS  near_max=1.0ms(<50ms reflex budget) far=301.0ms noninterference=yes separation=301x
```

| 量 | 反射層(near, node2) | 熟慮層(far, node3) |
|----|----------------------|----------------------|
| 往復遅延 RTT | **~1.0 ms**(min 0.0 / mean ~0.5) | **~300–301 ms** |
| 注入遅延 | 0 ms | 300 ms(`RELAY_FAR_DELAY_MS`) |
| デブリ締切 1000ms 内? | YES(余裕 ~1000×) | NO(締切超過) |
| 最後の echo 到達 | +~220 ms | +~301 ms |
| 時定数の分離 | — | **~300×** |

### 2.2 何が言えたか

- **(A) 遅延は実際に注入され、実測された。** far RTT ≈ 300ms は `RELAY_FAR_DELAY_MS`
  と一致。`locality.md (D)` の「per-packet 遅延を一切注入しない」を是正した
  ―― **これが本波の主眼**。relay の遅延キューは実 socket の往復で観測可能。
- **(B) far の遅延は near の即応性を損なわない(§8 の核心)。** far プローブが
  キューで 300ms 待つ間に撃った 12 発の near プローブは全て **~1ms で往復し、
  far より先に返った**(non-interference=yes)。near の RTT は反射予算 50ms にも
  デブリ締切 1000ms にも遠く及ばない速さを保つ。**二つの時定数は ~300× 分離**。
- **(C) なぜ二層が要るか(対偶)。** sim 側が示す: もし反射が大域(火星)の合意を
  待つ単層なら、その t63 は ~750s ―― デブリ締切 1000ms を **~750× 超過**。
  「秒速十数km のデブリには遠方の英知は間に合わない」(§8)が数で出る。

## 3. §8 の主張は数字で支持されたか

**YES(分離は実測で支持)/ ただし距離は依然モデル。** 正直に分ける。

- **支持された(実測)**: per-packet 遅延を実注入し、far=300ms / near=1ms の
  ~300× 分離を REAL relay 往復で測れた。near が far の遅延キューにブロックされない
  (non-interference)ことも実測。これが locality.md (D) の埋め合わせ。
- **モデルのまま**: `far_delay=300ms` という *値* は光速の壁の代理であって実距離では
  ない。実 Earth–Mars 片道は 3–22 分(§1.2)。300ms は localhost で観測可能な
  スケールへ *圧縮* したもの。距離 ∝ 遅延の対応自体は sim が計算するがハード非依存。

> まとめ: 「**遅延が反射層の即応性を損なわない**」という §10-B の確認項目は
> **実測で達成**。「光速の壁そのもの(分単位の実遅延)」は **モデル**のまま ――
> それは実フリート(§5)でしか測れない。

## 4. sim(モデル)の結果

`PKERNEL_SIM_NO_PNG=1 python3 tools/sim/latency_twolayer_sim.py`:

```
 far distance               reflex_t63   delib_t63        separation
 ------------------------------------------------------------------
 near region (RTT<=tau)         225 ms        2025 ms          9x
 Moon  (3.84e5 km)              225 ms        3282 ms         15x
 Mars min (5.46e7 km)           225 ms       184 s         818x
 Mars mean (2.25e8 km)          225 ms       753 s        3345x
 Mars max (4.01e8 km)           225 ms      1340 s        5954x

 [twolayer-sim immediacy]  PASS: 反射 t63 は far 遅延に対し不変 (spread=0.000 ms)
 [twolayer-sim separation] PASS: 熟慮 t63 (Mars max)=1340s > 締切 かつ > 10x reflex
```

掃引の核心: 遠方距離をどれだけ伸ばしても **反射層の t63 は 225ms で不変**
(spread = 0)。反射ループは近傍で閉じ、大域の光速遅延に乗らないから。熟慮層の
t63 は far に比例して伸び、Mars で分単位になる ―― だから熟慮を反射 path に置けない。
これは sim 上のモデル計算であり、§2 の実測(~300× 分離)の **設計根拠と期待値**を与える。

## 5. 実機(Android フリート)で測るべき TODO

本ハーネスの残る穴は §3 の「距離はモデル」。実フリートでは:

1. **実 RTT**: 地理的に離れたノード間の relay 往復を実測し、region(近傍)往復と
   cross-region(遠方)往復のレイテンシ差を実数で出す(300ms 代理を実値で置換)。
2. **実 τ**: SWIM の実測 RTT から `REGION_TAU_MS` を経験的に決め、反射層の
   実際の時間予算を測る([regions.md §3.1](../architecture/regions.md))。
3. **反射の実締切**: デブリ相当の時間制約イベントで、反射が実際に締切内に閉じるか。
4. **層またぎの発振**: [reflex-deliberation.md §7](../architecture/reflex-deliberation.md)
   の未解決(2 時定数の比、反射と熟慮の和解)を実遅延の下で観測。

## 6. 触ったファイル / 触っていないもの

- 追加/変更(本波): `relay/relay.c`(遅延ノブ + poll ループ)、
  `tools/sim/latency_twolayer_sim.py`(新規)、`samples/29_latency/`(新規)、
  本ドキュメント。
- **カーネル本体(`moe.c`/`region.c`/`swim.c` 等)は一切触っていない。** 時定数の
  定数(`tau_r`/`tau_d`/`REGION_TAU_MS`)は sim/sample 内で参照のみ(値はカーネルから
  写し、ズレ防止にコメント併記)。本波は relay とホスト計測に閉じる。
