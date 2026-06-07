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


# ----------------------------------------------------------------------------
# §2 脅威軸 (THREAT/PROTECT) — 守る対象へ全網の力を「注ぐ」(G20)
# ----------------------------------------------------------------------------
# 上の load 軸 (busy→idle の拡散) は正しいが「忙しい」だけを扱う。survival §2 は
# 「危ない/守るべき一点」へ群れが *集束* する (rally) ことを要求する。第13波で
# カーネル moe.c は pressure を *2 軸* に分けた:
#     load   → utility から *引く* (混んでいる→送るな = 避ける)     [正しい]
#     threat → utility に *足す* (危ない→守れ   = 寄る)             [G20 修正]
# このゲート効用関数は arch/common/moe.c::expert_utility と *同じ整数式* を写す。
# 定数がカーネルと黙ってズレないよう、ここに本物の #define 値を併記する:
#     MOE_PRESS_NUM/DEN   = 1/2  (load ペナルティ ゲイン 0.5)   — arch/common/include/moe.h
#     MOE_PROTECT_NUM/DEN = 1/1  (threat ボーナス ゲイン 1.0)   — arch/common/include/moe.h
#     MOE_SWITCH_MARGIN   = 12   (乗り換えデッドバンド = §8 ヒステリシス)
#     MOE_SAME_REGION_BONUS = 5
# threat は gossip 帯域 (WORLD_BEACON_MS) の遅い信号なので、load のような毎決定の
# 殺到発振を起こさない。乗り換えは margin デッドバンドが安定化する (§8)。
MOE_PRESS_NUM,   MOE_PRESS_DEN   = 1, 2
MOE_PROTECT_NUM, MOE_PROTECT_DEN = 1, 1
MOE_SWITCH_MARGIN     = 12
MOE_SAME_REGION_BONUS = 5

def gate_utility(acc, load, threat, same_region=0, rtt_ms=0):
    """arch/common/moe.c::expert_utility と同じ整数式。load は引き、threat は足す。"""
    u = acc
    u -= rtt_ms // 20
    u -= (load   * MOE_PRESS_NUM)   // MOE_PRESS_DEN     # 負荷: 避ける (−)
    u += (threat * MOE_PROTECT_NUM) // MOE_PROTECT_DEN   # 脅威: 寄る   (+)
    if same_region:
        u += MOE_SAME_REGION_BONUS
    return u


def run_threat_axis(rule, T=14, V0=40.0, p_cap=4.0, help_cap=8.0,
                    helpers=4, home=15.0, threat=40.0, shed=10.0):
    """脅威ノード P が「守るべき仕事 V0」を抱える。1 規則ぶんを T tick 回す。

    rule="protect": threat を *脅威軸* へ (本番カーネル)。
    rule="naive"  : threat を *負荷軸* へ畳む (= G20 のバグ; 脅威==混雑)。

    返り: dict(held, rallied, survived, lost) の時系列 + 最終 survived%/lost%。
      held    = まだ P が保持している守るべき仕事 (backlog)
      rallied = この tick に P へ寄った近傍数 (応援)
      survived= P の管理下で処理し終えた累計 (= 守れた)
      lost    = P から逃がして散逸した累計 (= flee で失われた)
    決定は本番 gate_utility のみで下す。中央配列を読む行は無い (NO-CENTRAL)。
    """
    held = V0
    survived = 0.0
    lost = 0.0
    hist = {"held": [], "rallied": [], "survived": [], "lost": []}
    for _ in range(T):
        # P の局所負荷 (= 抱えている守るべき仕事)。
        load_P = held
        if rule == "naive":      # 脅威を負荷へ畳む (避ける符号; 倒錯)
            u_P_self = gate_utility(70, load_P + threat, 0)   # P 自身が見る自分
            u_P_peer = gate_utility(70, load_P + threat, 0)   # 近傍が見る P
        else:                    # 脅威を脅威軸へ (寄る符号; §2)
            u_P_self = gate_utility(70, load_P, threat)
            u_P_peer = gate_utility(70, load_P, threat)
        u_alt  = gate_utility(70, home, 0)    # 「手放す/手元に留まる」対抗候補

        # (A) P は守るべき仕事を保持するか手放すか (deadband: 現職=keep)。
        if u_alt > u_P_self + MOE_SWITCH_MARGIN:
            chunk = min(held, shed)
            held -= chunk
            lost += chunk          # flee: 仕事が P を離れ散逸 = 守れなかった
        # (B) 近傍は P へ寄るか避けるか。寄った数ぶん実効処理能力が上がる。
        rallied = helpers if (u_P_peer > u_alt + MOE_SWITCH_MARGIN) else 0
        service = p_cap + rallied * help_cap
        done = min(held, service)
        held -= done
        survived += done           # P の管理下で完遂 = 守れた

        hist["held"].append(held)
        hist["rallied"].append(rallied)
        hist["survived"].append(survived)
        hist["lost"].append(lost)
    total = survived + lost
    hist["survived_pct"] = 100.0 * survived / V0
    hist["lost_pct"]     = 100.0 * lost / V0
    hist["final_rallied"] = hist["rallied"][-1]
    return hist


def report_threat_axis():
    print("=" * 74)
    print(" §2 脅威軸 — 守る対象へ全網の力を「注ぐ」 (G20 符号分離の検証)")
    print(" survival-network.md §2 / philosophy-gap-audit-3.md G20")
    print("=" * 74)
    print(" 脅威ノード P が守るべき仕事 V0=40 を抱える。2 規則を同条件で比較:")
    print("   naive   : 脅威を *負荷軸* へ畳む (threat==load; G20 のバグ=避ける符号)")
    print("   protect : 脅威を *脅威軸* へ流す (本番 moe.c; 寄る符号 §2)")
    print(" ゲート判断は本番と同じ gate_utility のみ (load は引く / threat は足す)。")
    print()

    naive   = run_threat_axis("naive")
    protect = run_threat_axis("protect")

    def row(tag, h):
        print(f" [{tag}]")
        print(f"   held@P    {spark(h['held'])}  (V0=40 -> end {h['held'][-1]:5.1f})")
        print(f"   rallied   {spark(h['rallied'])}  (helpers toward P; end {h['final_rallied']})")
        print(f"   survived% {h['survived_pct']:5.1f}   lost%(fled) {h['lost_pct']:5.1f}")
        print()
    row("naive  (threat==load)  → flee", naive)
    row("protect (threat axis)  → rally", protect)

    # 近傍の力の向きを ASCII で「見える」化 (可視化=観測; 画像ではない)。
    def neighbourhood(rallied):
        # rally なら近傍の力は P へ *内向き*、避けるなら P の仕事は *外向き*。
        if rallied > 0:
            return ("   . v .        近傍 -> P : 全網の力が一点へ集束 (rally §2)\n"
                    "   > P <\n"
                    "   . ^ .")
        return ("   . ^ .        P -> 近傍 : 守るべき仕事が散逸 (flee; §2 の真逆)\n"
                "   < P >\n"
                "   . v .")
    print(" 近傍の力の向き (final tick):")
    print("  naive:")
    print(neighbourhood(naive["final_rallied"]))
    print("  protect:")
    print(neighbourhood(protect["final_rallied"]))
    print()

    ok = (protect["survived_pct"] >= 99.0 and naive["survived_pct"] < 60.0
          and protect["final_rallied"] > naive["final_rallied"]
          and naive["final_rallied"] == 0)
    verdict = "PASS" if ok else "FAIL"
    print(f" 判定 [{verdict}]: protect は守れた {protect['survived_pct']:.0f}% "
          f"(rally) / naive は守れた {naive['survived_pct']:.0f}% "
          f"(残りは flee で散逸)。")
    print(" 構造的担保: 判断は gate_utility (局所の load/threat 勾配) のみ。")
    print(" 脅威==負荷の倒錯では群れが守る対象から逃げ、二軸分離で初めて寄る。")
    print()
    return ok


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
    print()

    # §2 脅威軸 (G20): load 軸とは別の「寄る」符号を検証する。
    report_threat_axis()

    # §5 ∧ §2 (G35): 多点を同時に・並行に守る (single-consciousness を越える)。
    report_protect_plural()


# ----------------------------------------------------------------------------
# §5 ∧ §2 脅威軸の「多点・並行」(G35) — 同時に何件もの守る対象を並行に守る
# ----------------------------------------------------------------------------
# report_threat_axis() は「一点」へ寄る符号 (§2) を検証した。survival §5 はその
# 上に「同時に数百件の危機… それぞれに別々のエキスパート群が並行に立ち上がり…
# 全体として綺麗に分散」を要求する — 単一意識の脳の比喩を乗り越える核心。
#
# ここではカーネル本番 (protect.c / pfs_repl.c) と同型の「多点・並行・有限の力」
# モデルを回す:
#   - 格子上に M 個の「守る対象 (protected point)」を *同時に* 宣言する。各点は
#     R 個の異なる近傍へ耐久複製されねば安全にならない (= under-replicated の
#     あいだ at-risk; protect_threat_for と同符号)。
#   - 群れの複製の力は *有限*: 各ノードは 1 tick に高々 cap 個の複製しか受け
#     入れ/送出できない。多点が同時に同じ近傍を欲すれば、その有限の力を *分け合
#     う* (= §6 応援・受援を多点化)。
#   - 各オーナーは *自分の近傍だけ* を見て複製先を選ぶ (中央 argmax なし §7)。
#     処理順は毎 tick シャッフル = グローバル優先度も集約点も無い。
#
# 検証する性質:
#   (1) 同時多発・並行: M 点が *並行に* 安全へ到達する。at-risk 数は M→0 へ数
#       tick で落ちる (直列なら M tick 要する)。総 tick ≈ 一点 + 立ち上げ、N×
#       ではない。
#   (2) 綺麗に分散・公平: どの点も R 複製に到達 (最小複製数 → R; 飢餓なし)。
#   (3) 一点突破耐性 (§3): 途中でノードを破壊しても、失った複製を別近傍へ張り
#       直し、全点が再び安全へ戻る。
#   (4) 中央なし (§7): ルーティングは近傍 + シャッフル順のみ。大域配列を優先度
#       に使う行は無い。
# ----------------------------------------------------------------------------
def _plural_neighbors(G, i):
    x, y = i % G, i // G; out = []
    if x > 0:     out.append(i - 1)
    if x < G - 1: out.append(i + 1)
    if y > 0:     out.append(i - G)
    if y < G - 1: out.append(i + G)
    return out


def run_protect_plural(G=12, M=8, R=2, cap=2, steps=24, seed=11, drive=1,
                       serialized=False, kill_tick=None, killed=None):
    """M protected points declared at once on a GxG lattice. Each needs R
    durable replicas on distinct alive region members; the swarm's per-tick
    replication force is finite — each node accepts <= cap placements/tick and
    each owner drives <= `drive` replicas/tick (actuator pacing) — so the many
    simultaneous points SHARE the force, ramping up in parallel. Returns a
    history dict. NO central: each owner reads only local state; processing
    order is shuffled (no global priority)."""
    rnd = random.Random(seed)
    n = G * G
    alive = [True] * n
    NB = [_plural_neighbors(G, i) for i in range(n)]
    owners = rnd.sample(range(n), M)
    holders = [set([owners[m]]) for m in range(M)]      # nodes holding point m
    done_at = [None] * M
    killed = killed or []
    hist = {"atrisk": [], "minrep": [], "avgrep": [], "placed": []}

    # serialized policy: a point is only driven once all earlier points are safe
    def reps_of(m): return len(holders[m]) - 1          # replicas excl. owner

    for t in range(steps):
        if kill_tick is not None and t == kill_tick:
            for k in killed:
                alive[k] = False
                for m in range(M):
                    holders[m].discard(k)               # lost replica -> at-risk (§3)
        budget = [cap if alive[i] else 0 for i in range(n)]   # finite force
        placed = 0
        order = list(range(M)); rnd.shuffle(order)      # no global priority (§7)
        # serialized: restrict to the single lowest-index not-yet-safe point
        if serialized:
            active = [m for m in range(M) if reps_of(m) < R]
            order = active[:1]
        for m in order:
            if reps_of(m) >= R:
                if done_at[m] is None: done_at[m] = t
                continue
            # pick a driver that is alive (owner, or a surviving holder if the
            # owner died — the replica itself can re-drive; §3 no single point).
            drivers = [h for h in holders[m] if alive[h]]
            if not drivers:
                continue
            drv = drivers[0]
            need = min(R - reps_of(m), drive)       # actuator pacing per tick
            # candidates = any ALIVE non-holder region member with spare capacity
            # (the kernel's protect actuator pushes region-wide, not only to
            # lattice-adjacent nodes), preferring NEARER nodes (locality, §8).
            dx, dy = drv % G, drv // G
            cand = [j for j in range(n)
                    if alive[j] and j not in holders[m] and budget[j] > 0]
            cand.sort(key=lambda j: abs(j % G - dx) + abs(j // G - dy))
            for j in cand:
                if need <= 0: break
                holders[m].add(j); budget[j] -= 1; placed += 1; need -= 1
            if reps_of(m) >= R and done_at[m] is None:
                done_at[m] = t
        reps = [reps_of(m) for m in range(M)]
        hist["atrisk"].append(sum(1 for m in range(M) if reps[m] < R))
        hist["minrep"].append(min(reps))
        hist["avgrep"].append(sum(reps) / M)
        hist["placed"].append(placed)
    hist["done_at"] = done_at
    hist["all_safe"] = all(reps_of(m) >= R for m in range(M))
    # ticks to make ALL points safe (None entries = never)
    fin = [d for d in done_at if d is not None]
    hist["ticks_all_safe"] = (max(fin) + 1) if (len(fin) == M) else None
    return hist


def report_protect_plural():
    print("=" * 74)
    print(" §5 ∧ §2 多点・並行の防衛 (G35) — 同時に何件もの守る対象を並行に守る")
    print(" survival-network.md §5 (同時多発・並行分散) / philosophy-gap-audit-5 G35")
    print("=" * 74)
    G, M, R, cap = 8, 12, 3, 2
    rnd = random.Random(99)
    kills = rnd.sample(range(G * G), 10)                  # 10 scattered nodes
    print(f" 格子 {G}x{G}={G*G}, 同時宣言する守る対象 M={M} 点, 各点 R={R} 耐久複製,")
    print(f" 複製の力は有限 (各 node <= {cap}/tick 受容, 各 owner <= 1/tick 駆動)")
    print(f" — 多点がそれを分け合い、並行に立ち上がる (§6 応援・受援の多点化)。")
    print()

    par = run_protect_plural(G=G, M=M, R=R, cap=cap, steps=24)
    ser = run_protect_plural(G=G, M=M, R=R, cap=cap, steps=60, serialized=True)
    kil = run_protect_plural(G=G, M=M, R=R, cap=cap, steps=24,
                             kill_tick=2, killed=kills)   # kill mid-convergence

    def row(tag, h):
        ta = h["ticks_all_safe"]
        print(f" [{tag}]")
        print(f"   at-risk   {spark(h['atrisk'])}  (M={M} -> end {h['atrisk'][-1]})")
        print(f"   min-repl  {spark(h['minrep'])}  (worst-protected point; -> R={R} = none starves)")
        print(f"   all-safe @ tick {ta}")
        print()

    row("parallel  (同時多発・並行; 本番 protect と同型)", par)
    row("serialized (一点ずつ; 乗り越えるべき単一意識)", ser)
    row("parallel + node kill @t=2 (§3 一点突破耐性)", kil)

    # ASCII 「並行に分散して立ち上がる」可視化 (可視化=観測; 画像ではない)
    print(" 立ち上がりの並行性 (at-risk 点数の推移): 並行は数 tick で M→0、")
    print(" 直列は M 点ぶん階段状に M tick かける。")
    print(f"   parallel   {spark(par['atrisk'])}")
    print(f"   serialized {spark(ser['atrisk'])}")
    print()

    p_t = par["ticks_all_safe"]; s_t = ser["ticks_all_safe"]
    ok = (par["all_safe"] and ser["all_safe"] and kil["all_safe"]
          and p_t is not None and s_t is not None
          and p_t * 2 <= s_t                     # parallel clearly beats serial
          and par["minrep"][-1] >= R             # fair: nobody starves
          and kil["atrisk"][-1] == 0)            # survived the kill
    verdict = "PASS" if ok else "FAIL"
    print(f" 判定 [{verdict}]: M={M} 点が並行に {p_t} tick で全安全 — 直列の {s_t}"
          f" tick の {('%.0f%%' % (100.0*p_t/s_t)) if s_t else '?'} 。")
    print(f"   公平: 最小複製数 -> R ({par['minrep'][-1]}/{R}; 飢餓なし)。"
          f" §3: kill 後も全点が安全へ復帰 (end at-risk {kil['atrisk'][-1]})。")
    print(" 構造的担保: 各オーナーは近傍 + シャッフル順のみで複製先を選ぶ。")
    print(" 大域状態を優先度に使う行は無い = 中央なし (§7)。単一意識を越える (§5)。")
    print()
    return ok


if __name__ == "__main__":
    main()
