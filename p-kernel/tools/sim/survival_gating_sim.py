#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
survival_gating_sim.py — §10 ステップA: 分散ゲーティング／応援・受援のミニ検証

docs/architecture/survival-network.md の構想を、最小スケールで「目で確認」する
ためのホスト・シミュレーション。カーネル本体(moe.c)に手を入れる前に、

    「中央コントローラなしに、局所勾配だけで負荷が逼迫点から余力点へ流れ、
      同時多発の危機が並行に解消し、ノードを壊しても全体が生き残る」

という構想の核心(§5/§7/§8/§3)が本当に成立するかを確かめる。

ゼロ依存。Python 標準ライブラリ(zlib)だけで PNG を吐く。p-kernel の文化に従う。

― モデル ―――――――――――――――――――――――――――――――――――――
 ・GxG の2D格子。各ノード i は容量 cap_i と 仕事キュー load_i を持つ。
 ・近傍 = 4近傍(von Neumann)。これが「光速で届く範囲」(§8 反射層)の抽象。
 ・局所状態  pressure_i = load_i - cap_i      (>0 逼迫 / <0 余力)
 ・応援・受援(§6,§7): 圧の離散ラプラシアン拡散。各ノードは *近傍の pressure だけ*
   を読み、自分より圧の低い隣へ仕事を流す。大域状態は一切読まない(=中央なし)。
       p_i += k * Σ_{j∈N(i), alive}(p_j - p_i)
   k は応援ゲイン。k を上げ過ぎると過剰応援で発振する(§8 が予言)。減衰=小さい k と
   反射層の局所閉ループが、これを収束に変える。
 ・サービス: 各ノードは毎tick cap_i だけ仕事を処理して load を減らす。
 ・危機: ある時刻に複数地点へ同時に大きな load を注入する(§5)。

決定関数 step_mutual_aid() が読むのは node.neighbors だけ。グローバル配列を
ルーティング判断に使う行は存在しない ―― これが「構造的に中央が無い」ことの担保。
"""

import zlib, struct, random, math, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))

# ----------------------------------------------------------------------------
# ゼロ依存 PNG ライター (truecolor, filter 0)
# ----------------------------------------------------------------------------
def write_png(path, w, h, pixels):
    """pixels: bytearray/bytes of length w*h*3 (RGB, row-major)."""
    def chunk(typ, data):
        return (struct.pack(">I", len(data)) + typ + data +
                struct.pack(">I", zlib.crc32(typ + data) & 0xffffffff))
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)                       # filter type 0 per scanline
        raw += pixels[y * stride:(y + 1) * stride]
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)   # 8-bit truecolor
    idat = zlib.compress(bytes(raw), 9)
    with open(path, "wb") as f:
        f.write(sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b""))


class Canvas:
    def __init__(self, w, h, bg=(248, 248, 248)):
        self.w, self.h = w, h
        self.px = bytearray(bg * (w * h))
    def rect(self, x0, y0, ww, hh, color):
        r, g, b = color
        x1, y1 = min(x0 + ww, self.w), min(y0 + hh, self.h)
        x0, y0 = max(x0, 0), max(y0, 0)
        for y in range(y0, y1):
            base = (y * self.w + x0) * 3
            for _ in range(x0, x1):
                self.px[base] = r; self.px[base + 1] = g; self.px[base + 2] = b
                base += 3
    def save(self, path):
        write_png(path, self.w, self.h, self.px)


def heat(t):
    """load ratio t∈[0,1] → 青(余力)→黄→赤(逼迫)。"""
    t = 0.0 if t < 0 else (1.0 if t > 1 else t)
    if t < 0.5:
        s = t / 0.5
        return (int(30 + s * 210), int(70 + s * 150), int(200 - s * 160))   # blue→yellow
    s = (t - 0.5) / 0.5
    return (int(240 - s * 30), int(220 - s * 190), int(40 - s * 10))         # yellow→red

DEAD = (24, 24, 24)


# ----------------------------------------------------------------------------
# モデル
# ----------------------------------------------------------------------------
class Grid:
    def __init__(self, G, cap=10.0, seed=1):
        self.G = G
        self.n = G * G
        rnd = random.Random(seed)
        # 容量は不均一(弱いノードも混ざる)が、ネットワークが補う前提(§2,§3)
        self.cap = [cap * (0.6 + 0.8 * rnd.random()) for _ in range(self.n)]
        self.load = [0.0] * self.n
        self.alive = [True] * self.n
        self.neigh = [self._neighbors(i) for i in range(self.n)]

    def _neighbors(self, i):
        G = self.G; x, y = i % G, i // G; out = []
        if x > 0:     out.append(i - 1)
        if x < G - 1: out.append(i + 1)
        if y > 0:     out.append(i - G)
        if y < G - 1: out.append(i + G)
        return out

    def inject(self, idx, amount):
        if self.alive[idx]:
            self.load[idx] += amount

    def kill(self, idx):
        # ノード破壊: 抱えていた仕事も失われる(過酷な現実)。隣が穴を埋める。
        self.alive[idx] = False
        self.load[idx] = 0.0

    # --- 応援・受援: 中央なしの局所勾配ステップ ----------------------------
    # 読むのは self.neigh[i] と、その隣の pressure だけ。大域は読まない。
    def step_mutual_aid(self, k):
        if k <= 0.0:
            return
        load, cap, alive, neigh = self.load, self.cap, self.alive, self.neigh
        pres = [ (load[i] - cap[i]) if alive[i] else 0.0 for i in range(self.n) ]
        delta = [0.0] * self.n
        for i in range(self.n):
            if not alive[i]:
                continue
            pi = pres[i]
            for j in neigh[i]:
                if alive[j] and pres[j] < pi:        # 自分より楽な隣へ
                    flow = k * (pi - pres[j])         # 勾配に比例(対称: i→j のみ計上, j側は別途)
                    delta[i] -= flow
                    delta[j] += flow
        for i in range(self.n):
            if alive[i]:
                load[i] += delta[i]
                if load[i] < 0.0:
                    load[i] = 0.0

    def step_service(self):
        load, cap, alive = self.load, self.cap, self.alive
        for i in range(self.n):
            if alive[i]:
                load[i] -= cap[i]
                if load[i] < 0.0:
                    load[i] = 0.0

    # --- メトリクス ---
    def backlog(self):  return sum(self.load[i] for i in range(self.n) if self.alive[i])
    def maxq(self):     return max((self.load[i] for i in range(self.n) if self.alive[i]), default=0.0)
    def overloaded(self):
        return sum(1 for i in range(self.n) if self.alive[i] and self.load[i] > self.cap[i])


# ----------------------------------------------------------------------------
# シナリオ実行
# ----------------------------------------------------------------------------
def run(scenario, G=26, k=0.18, steps=46, seed=7, crises=None, kills=None,
        snapshots=None):
    """1シナリオを回し、(grid列, スナップショット負荷, メトリクス) を返す。"""
    g = Grid(G, seed=seed)
    crises = crises or {}
    kills = kills or {}
    snapshots = snapshots or []
    snaps, hist = {}, {"backlog": [], "maxq": [], "over": []}
    for t in range(steps):
        for (cx, cy, rad, amt) in crises.get(t, []):
            for i in range(g.n):
                x, y = i % G, i // G
                if abs(x - cx) + abs(y - cy) <= rad:
                    g.inject(i, amt)
        for (cx, cy, rad) in kills.get(t, []):
            for i in range(g.n):
                x, y = i % G, i // G
                if abs(x - cx) + abs(y - cy) <= rad:
                    g.kill(i)
        if scenario != "baseline":
            g.step_mutual_aid(k)
        g.step_service()
        hist["backlog"].append(g.backlog())
        hist["maxq"].append(g.maxq())
        hist["over"].append(g.overloaded())
        if t in snapshots:
            snaps[t] = (list(g.load), list(g.alive))
    return g, snaps, hist


# ----------------------------------------------------------------------------
# 可視化: 行=シナリオ, 列=時刻 のコンタクトシート
# ----------------------------------------------------------------------------
def render_contact_sheet(path, G, rows, snap_times, vmax, cell=9, gap=2,
                         pad=14, rowgap=16, gamma=0.5):
    # γ<1 で低負荷を持ち上げ、baseline の鋭いスパイクに潰されず aid の浅く広い
    # ブルーム(=応援の拡散そのもの)が見えるようにする。色基準(vmax,γ)は全行共通。
    panel = G * cell + (G - 1) * gap
    ncol = len(snap_times)
    W = pad * 2 + ncol * panel + (ncol - 1) * pad
    H = pad * 2 + len(rows) * panel + (len(rows) - 1) * rowgap
    cv = Canvas(W, H)
    for r, (_label, snaps) in enumerate(rows):
        py = pad + r * (panel + rowgap)
        for c, t in enumerate(snap_times):
            px = pad + c * (panel + pad)
            load, alive = snaps[t]
            for i in range(G * G):
                x, y = i % G, i // G
                ratio = (max(load[i], 0.0) / vmax) ** gamma
                col = DEAD if not alive[i] else heat(ratio)
                cv.rect(px + x * (cell + gap), py + y * (cell + gap), cell, cell, col)
    cv.save(path)
    return W, H


# ----------------------------------------------------------------------------
# コンソール用スパークライン
# ----------------------------------------------------------------------------
BARS = "▁▂▃▄▅▆▇█"
def spark(series):
    lo, hi = min(series), max(series)
    if hi - lo < 1e-9:
        return BARS[0] * len(series)
    return "".join(BARS[min(7, int((v - lo) / (hi - lo) * 7.999))] for v in series)


def main():
    G = 26
    # 同時多発の危機(§5): 鋭い一点集中。1ノードへ独力では捌けない量を同時注入する。
    # cap≈10/tick に対し 180 → 単独なら ~18tick 居座る。応援があれば近傍へ拡散して速く消える。
    crises = {
        2:  [(5, 6, 0, 180.0), (20, 4, 0, 180.0), (7, 20, 0, 180.0), (19, 19, 0, 180.0)],
        16: [(11, 15, 0, 200.0), (15, 11, 0, 200.0)],   # 第2波: 破壊された死帯の隣で発生
    }
    # 一点突破耐性(§3): t=9 に格子中央のノード群を破壊。第2波(t=16)はこの死帯の
    # すぐ脇で起き、応援は死帯を迂回して流れねばならない。
    kills = {9: [(13, 13, 2)]}
    snap_times = [3, 6, 12, 18, 34]
    steps = 46
    K = 0.16

    print("=" * 74)
    print(" §10 ステップA — 分散ゲーティング/応援・受援 ミニ検証")
    print(" docs/architecture/survival-network.md  (§5 同時多発 / §7 中央なし勾配 /")
    print("  §8 二層・時定数 / §3 一点突破耐性)")
    print("=" * 74)
    print(f" 格子 {G}x{G}={G*G} ノード, 4近傍, 容量は不均一(0.6〜1.4x)")
    print(f" 危機注入 t={sorted(crises)} (同時多発), ノード破壊 t={sorted(kills)}")
    print()

    # 3シナリオ
    base_g, base_s, base_h = run("baseline", G=G, steps=steps, crises=crises,
                                 snapshots=snap_times)
    aid_g,  aid_s,  aid_h  = run("aid",      G=G, k=K, steps=steps, crises=crises,
                                 snapshots=snap_times)
    kill_g, kill_s, kill_h = run("aid",      G=G, k=K, steps=steps, crises=crises,
                                 kills=kills, snapshots=snap_times)

    # 全シナリオ・全スナップ共通の vmax(行間で色を比較可能に)
    vmax = 1e-6
    for snaps in (base_s, aid_s, kill_s):
        for t in snap_times:
            vmax = max(vmax, max(snaps[t][0]))
    vmax = max(vmax, 1.0)

    # PNG はリサーチ用の補助出力(主たる可観測量はこの下の ASCII スパークライン)。
    # CI/ヘッドレス環境では PKERNEL_SIM_NO_PNG=1 でスキップできる。
    out = os.path.join(HERE, "out_gating.png")
    W = H = 0
    if not os.environ.get("PKERNEL_SIM_NO_PNG"):
        W, H = render_contact_sheet(
            out, G,
            [("baseline (応援なし)", base_s),
             ("local-gradient aid (§7)", aid_s),
             ("aid + node kills (§3)", kill_s)],
            snap_times, vmax)

    def report(name, h):
        print(f" [{name}]")
        print(f"   backlog   {spark(h['backlog'])}  (max {max(h['backlog']):7.1f} → end {h['backlog'][-1]:6.1f})")
        print(f"   max-queue {spark(h['maxq'])}  (peak {max(h['maxq']):7.1f} → end {h['maxq'][-1]:6.1f})")
        print(f"   overloaded{spark(h['over'])}  (peak {max(h['over'])} → end {h['over'][-1]})")
        print()
    report("baseline 応援なし", base_h)
    report("local-gradient aid §7", aid_h)
    report("aid + kills §3", kill_h)

    # §8: 発振 vs 減衰 ― 同一の単発ショックで応援ゲイン k を変える
    print(" §8 二層/時定数の検証 ― 単発ショックでの応援ゲイン k の効き:")
    shock = {1: [(13, 13, 2, 60.0)]}
    for tag, kk in [("k=0.05 (強減衰)", 0.05), ("k=0.18 (適正)", 0.18),
                    ("k=0.55 (過剰応援→発振)", 0.55)]:
        _, _, h = run("aid", G=G, k=kk, steps=30, crises=shock)
        print(f"   {tag:24s} max-queue {spark(h['maxq'])}  end {h['maxq'][-1]:6.2f}")
    print()
    if W:
        print(f" → PNG: {out}  ({W}x{H})")
    else:
        print(" → PNG: skipped (PKERNEL_SIM_NO_PNG); ASCII sparklines above are the primary signal")
    print("   行: baseline / local-gradient aid / aid+kills   列: t=", snap_times)
    print()
    print(" 構造的担保: ルーティング判断 Grid.step_mutual_aid() が読むのは")
    print(" self.neigh[i] と近傍の pressure のみ。大域状態を見る行は存在しない=中央なし。")


if __name__ == "__main__":
    main()
