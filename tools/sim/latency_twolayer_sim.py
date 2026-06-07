#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
latency_twolayer_sim.py — §10 ステップB: 二層構造を光速遅延の下で検証する

docs/architecture/survival-network.md の §4(MoE スパース性 = 光速/エネルギー
制約への答え)と §8(反射層/熟慮層の二層構造)を、最小モデルで「数」にする。
俯瞰監査 G31「§4 の核(latency/light-speed)が未計測」への回答の *モデル側*。

問い(§10 ステップB を逐語):
    「ノード間通信に遅延(距離に比例)を導入。反射層: 近傍のみ・低遅延・即応。
      熟慮層: 全体・高遅延・深い判断/更新。
      *遅延が反射層の即応性を損なわないことを確認*。」

このスクリプトが示すこと:
  (1) 距離→遅延(光速)モデルの下で、反射層(近傍・低遅延)は速い時定数で即応し、
      熟慮層(全体・高遅延)は遅れて立ち上がる ―― 二つの時定数が分離している。
  (2) *遠方ノードの遅延をいくら注入しても、反射層の即応性(t63)は一定*。
      反射ループは近傍で閉じ、大域(光速)の遅延に乗らないから(§8 の核心)。
  (3) もし単層で「近傍が大域の合意を待つ」設計にすると、反射は火星の光が届くまで
      動けず、デブリ(秒速十数km)には間に合わない ―― なぜ二層が要るかの対偶。

―― 何がモデルで何が実測か(正直に) ――――――――――――――――――――――
  ・モデル : 光速遅延(delay = 距離 / c)、距離、一次系の時定数応答。
             実ハードウェアも実無線も使わない。これは *概念の検証* である。
  ・実測  : リアルな socket 往復遅延は samples/29_latency/ が REAL relay の
             遅延ノブ(RELAY_FAR_DELAY_MS)で測る。本 sim はその設計根拠を与える。

ゼロ依存。ASCII スパークラインが主たる可観測量。PNG は補助(PKERNEL_SIM_NO_PNG
で無効化・.gitignore 済み)。p-kernel の文化(可視化=観測, 画像は二次)に従う。
"""

import math, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))

# ----------------------------------------------------------------------------
# 物理定数・カーネル時定数(カーネルとズレないよう #define 値を併記)
# ----------------------------------------------------------------------------
C_KM_S = 299792.458              # 光速 (km/s)

# region.h / regions.md: REGION_TAU_MS = 50  → 近傍は相互 RTT ≤ 50ms。
# 片道に直すと ~25ms。これが「光速で間に合う範囲」(§8 反射層)の工学的定義。
REGION_TAU_MS   = 50
NEAR_ONEWAY_MS  = REGION_TAU_MS / 2.0     # 25 ms

# reflex-deliberation.md / regions: 反射 ~200ms, 熟慮 ~2s, gossip ~5s。
# D2 の実測比(fast=4 / slow=40 tick = 10×)とも整合する時定数の比。
TAU_REFLEX_MS = 200.0           # 反射層の時定数(速い)
TAU_DELIB_MS  = 2000.0          # 熟慮層の時定数(遅い) = 10× 反射
GOSSIP_MS     = 5000.0          # 大域 gossip 周期(参考。熟慮の更新帯域)

# §8: 秒速十数 km のデブリ。検知から回避までの物理的締切。
# 「間に合わなければ質はゼロ」(reflex-deliberation.md §1)の締切を 1 秒に置く。
DEBRIS_DEADLINE_MS = 1000.0

# 距離カタログ(片道光遅延の出どころ)。near は距離ではなく RTT≤τ で定義。
DISTANCES_KM = [
    ("near region (RTT<=tau)",      None),          # 近傍: 距離でなく τ で定義
    ("Moon  (3.84e5 km)",           384_400.0),
    ("Mars min (5.46e7 km)",        54_600_000.0),
    ("Mars mean (2.25e8 km)",       225_000_000.0),
    ("Mars max (4.01e8 km)",        401_000_000.0),
]

def light_delay_ms(dist_km):
    """距離→片道光遅延 (ms)。これがモデルの核(distance ∝ delay)。"""
    return (dist_km / C_KM_S) * 1000.0

def comm_oneway_ms(dist_km):
    """そのリンクの片道通信遅延。near は τ/2、far は光速遅延。"""
    return NEAR_ONEWAY_MS if dist_km is None else light_delay_ms(dist_km)


# ----------------------------------------------------------------------------
# 一次系のステップ応答 ―― 各層の「立ち上がり」
# ----------------------------------------------------------------------------
# 脅威(デブリ検知)を t=0 のステップ入力とする。ある層が反応を *開始* できるのは
# 入力信号がその層に届いてから(= t_arrive = 通信遅延)。届いた後は時定数 tau の
# 一次系で立ち上がる:  y(t) = 1 - exp(-(t - t_arrive)/tau)   (t >= t_arrive)
def response(t_ms, t_arrive_ms, tau_ms):
    if t_ms <= t_arrive_ms:
        return 0.0
    return 1.0 - math.exp(-(t_ms - t_arrive_ms) / tau_ms)

def t63_ms(t_arrive_ms, tau_ms):
    """応答が 63% (時定数 1 本ぶん) に達する時刻 = 到達遅延 + tau。"""
    return t_arrive_ms + tau_ms


# ----------------------------------------------------------------------------
# ASCII スパークライン
# ----------------------------------------------------------------------------
BARS = "▁▂▃▄▅▆▇█"
def spark_curve(t_arrive, tau, t0, t1, n=40):
    """時間窓 [t0,t1] を n 点サンプルした応答曲線のスパークライン。"""
    out = []
    for k in range(n):
        t = t0 + (t1 - t0) * k / (n - 1)
        out.append(response(t, t_arrive, tau))
    return "".join(BARS[min(7, int(v * 7.999))] for v in out)


# ----------------------------------------------------------------------------
# 補助 PNG(二次出力。主たる信号は上の ASCII)
# ----------------------------------------------------------------------------
def try_write_png(reflex_t63, sweep):
    if os.environ.get("PKERNEL_SIM_NO_PNG"):
        return None
    try:
        import zlib, struct
    except Exception:
        return None
    W, H, pad = 560, 300, 40
    px = bytearray((250, 250, 250) * (W * H))
    def put(x, y, c):
        if 0 <= x < W and 0 <= y < H:
            i = (y * W + x) * 3
            px[i], px[i+1], px[i+2] = c
    def bar(x0, w, h, c):
        for x in range(x0, min(x0 + w, W)):
            for y in range(max(H - pad - h, 0), H - pad):
                put(x, y, c)
    # log-scale バー: 各距離の reflex_t63(一定)と delib_t63(伸びる)を比較
    vals = [(lbl, reflex_t63, dt63) for (lbl, _d, _c, _r, dt63) in sweep]
    mx = max(max(r, d) for _l, r, d in vals)
    def hpx(v):
        return int((H - 2 * pad) * (math.log10(v + 1) / math.log10(mx + 1)))
    n = len(vals); slot = (W - 2 * pad) // n
    for i, (_l, r, d) in enumerate(vals):
        x0 = pad + i * slot
        bar(x0 + 2,           slot // 2 - 3, hpx(r), (70, 130, 200))   # reflex (青)
        bar(x0 + slot // 2,   slot // 2 - 3, hpx(d), (210, 80, 60))    # delib (赤)
    def chunk(typ, data):
        return (struct.pack(">I", len(data)) + typ + data +
                struct.pack(">I", zlib.crc32(typ + data) & 0xffffffff))
    raw = bytearray()
    for y in range(H):
        raw.append(0); raw += px[y * W * 3:(y + 1) * W * 3]
    out = os.path.join(HERE, "out_latency_twolayer.png")
    with open(out, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
                + chunk(b"IEND", b""))
    return out


# ----------------------------------------------------------------------------
# メイン
# ----------------------------------------------------------------------------
def main():
    print("=" * 76)
    print(" §10 ステップB — 二層構造を光速遅延の下で検証 (距離→遅延モデル)")
    print(" docs/architecture/survival-network.md §4 (MoE/光速) / §8 (反射・熟慮)")
    print(" docs/architecture/reflex-deliberation.md (時定数の分離)")
    print("=" * 76)
    print(f" 反射時定数 tau_r = {TAU_REFLEX_MS:.0f} ms   熟慮時定数 tau_d = {TAU_DELIB_MS:.0f} ms"
          f"   (比 {TAU_DELIB_MS/TAU_REFLEX_MS:.0f}x)")
    print(f" 近傍片道 = tau/2 = {NEAR_ONEWAY_MS:.0f} ms (REGION_TAU_MS={REGION_TAU_MS})"
          f"   デブリ締切 = {DEBRIS_DEADLINE_MS:.0f} ms")
    print(f" 光速 c = {C_KM_S:.0f} km/s   遅延 = 距離 / c  (← これがモデルの核)")
    print()

    # --- (0) 距離→光速遅延の表 ----------------------------------------------
    print(" [距離 → 片道光速遅延] (モデル)")
    for lbl, dist in DISTANCES_KM:
        if dist is None:
            print(f"   {lbl:28s} {NEAR_ONEWAY_MS:10.1f} ms (= tau/2, 距離でなく RTT で定義)")
        else:
            d = light_delay_ms(dist)
            print(f"   {lbl:28s} {d:10.1f} ms  ({d/1000:.1f} s / {d/60000:.2f} min)")
    print()

    # --- (1) 二層の応答曲線(代表: 火星平均距離) -----------------------------
    mars_mean = comm_oneway_ms(225_000_000.0)
    print(" [二層の立ち上がり] 脅威を t=0 に注入。反射=近傍・速い窓 / 熟慮=火星平均・遅い窓")
    print(f"   (反射の窓 0..{int(3*TAU_REFLEX_MS)} ms / 熟慮の窓 0..{int(mars_mean+3*TAU_DELIB_MS)} ms。")
    print( "    各行は自前の時間窓。█ = 100% 応答に到達。)")
    refl_win = 3 * TAU_REFLEX_MS
    delib_win = mars_mean + 3 * TAU_DELIB_MS
    print(f"   反射層 (near, 即応) {spark_curve(NEAR_ONEWAY_MS, TAU_REFLEX_MS, 0, refl_win)}"
          f"  t63={t63_ms(NEAR_ONEWAY_MS, TAU_REFLEX_MS):.0f} ms")
    print(f"   熟慮層 (Mars, 熟考){spark_curve(mars_mean, TAU_DELIB_MS, 0, delib_win)}"
          f"  t63={t63_ms(mars_mean, TAU_DELIB_MS)/1000:.0f} s")
    print(f"   → 反射は {t63_ms(NEAR_ONEWAY_MS,TAU_REFLEX_MS):.0f} ms で締切 {DEBRIS_DEADLINE_MS:.0f} ms 内に立つ。"
          f"熟慮は {t63_ms(mars_mean,TAU_DELIB_MS)/1000:.0f} s ―― 同じループには絶対に乗せられない。")
    print()

    # --- (2) 核心: 遠方遅延を掃引しても反射の即応性は不変 -------------------
    print(" [§8 の核心] 遠方ノードの遅延を掃引 → 反射層の t63 は一定か?")
    print("   反射ループは近傍で閉じる(大域の光速遅延に乗らない)はず。")
    print()
    print("   far distance               reflex_t63   delib_t63        separation")
    print("   " + "-" * 66)
    reflex_t63 = t63_ms(NEAR_ONEWAY_MS, TAU_REFLEX_MS)   # 反射は far に依存しない
    sweep = []
    for lbl, dist in DISTANCES_KM:
        comm = comm_oneway_ms(dist)
        dt63 = t63_ms(comm, TAU_DELIB_MS)
        sep = dt63 / reflex_t63
        sweep.append((lbl, dist, comm, reflex_t63, dt63))
        d_disp = f"{dt63:9.0f} ms" if dt63 < 10000 else f"{dt63/1000:7.0f} s "
        print(f"   {lbl:26s} {reflex_t63:7.0f} ms   {d_disp}   {sep:8.0f}x")
    print()

    # --- 判定 ----------------------------------------------------------------
    reflex_vals = [r for (_l, _d, _c, r, _dt) in sweep]
    reflex_spread = max(reflex_vals) - min(reflex_vals)
    immediacy_ok = (reflex_spread < 1e-6) and (reflex_t63 <= DEBRIS_DEADLINE_MS)
    # 熟慮側は far が増えると確実に締切を超える(= reflex path に置けない証拠)
    delib_far = sweep[-1][4]
    separation_ok = delib_far > DEBRIS_DEADLINE_MS and delib_far > 10 * reflex_t63

    v1 = "PASS" if immediacy_ok else "FAIL"
    print(f" [twolayer-sim immediacy] {v1}: 反射 t63 は far 遅延に対し不変 "
          f"(spread={reflex_spread:.3f} ms) かつ締切内 ({reflex_t63:.0f} <= {DEBRIS_DEADLINE_MS:.0f} ms)。")
    v2 = "PASS" if separation_ok else "FAIL"
    print(f" [twolayer-sim separation] {v2}: 熟慮 t63 (far=Mars max) = {delib_far/1000:.0f} s "
          f"> 締切 {DEBRIS_DEADLINE_MS/1000:.0f} s かつ > 10x reflex。二つの時定数は分離。")
    print()

    # --- (3) 対偶: 単層(近傍が大域合意を待つ)だと反射は死ぬ ----------------
    print(" [なぜ二層か(対偶)] もし反射が大域(火星)の合意を待つ単層なら:")
    naive_t63 = t63_ms(mars_mean, TAU_REFLEX_MS)   # 速い時定数でも到達は火星遅延に縛られる
    print(f"   反射の窓 0..{int(refl_win)} ms で観た単層の応答 "
          f"{spark_curve(mars_mean, TAU_REFLEX_MS, 0, refl_win)}  (全くゼロ)")
    print(f"   → 単層 t63 = {naive_t63/1000:.0f} s 。デブリ締切 {DEBRIS_DEADLINE_MS:.0f} ms を"
          f" {naive_t63/DEBRIS_DEADLINE_MS:.0f}x 超過 ―― 手は引っ込められない。")
    print(f"   反射を近傍で閉じて初めて {reflex_t63:.0f} ms に立つ。これが §8 の存在理由。")
    print()

    out = try_write_png(reflex_t63, sweep)
    if out:
        print(f" → PNG(補助): {out}  (青=reflex_t63 一定 / 赤=delib_t63 伸長, log軸)")
    else:
        print(" → PNG: skipped (PKERNEL_SIM_NO_PNG)。上の ASCII が主たる信号。")
    print()
    print(" 正直な範囲: 光速遅延・距離・一次系時定数はすべて *モデル*。実ハード/実無線は")
    print(" 使っていない。リアルな socket 往復遅延は samples/29_latency が REAL relay の")
    print(" 遅延ノブで *実測* する。本 sim はその設計根拠と期待値を与えるもの。")

    return 0 if (immediacy_ok and separation_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
