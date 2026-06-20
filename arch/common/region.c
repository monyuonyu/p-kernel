/*
 *  region.c
 *  Regions — 遅延クラスタによる脳の領域分割 (R0)
 *
 *  設計: docs/architecture/regions.md
 *  詳細は region.h を参照。
 */

#include "region.h"
#include "swim.h"     /* swim_rtt_ms() */
#include "kernel.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ------------------------------------------------------------------ */
/* 出力ヘルパー                                                        */
/* ------------------------------------------------------------------ */

static void rg_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void rg_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { rg_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    rg_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* 状態 (最後に再計算した自 region の局所ビュー)                       */
/* ------------------------------------------------------------------ */

static UB member[DNODE_MAX];   /* 1 = 自 region のメンバ        */
static UB member_cnt = 0;      /* メンバ数 (自分を含む)         */
static UB coord      = 0xFF;   /* coordinator = メンバ内最小 ID */

/* ------------------------------------------------------------------ */
/* Supernode capability (N-2, p2p-overlay.md "Supernodes (N-2)")       */
/*                                                                     */
/* A node is "supernode-capable" iff it is reachable (not             */
/* symmetric-NAT'd) AND volunteered. In THIS slice capability is a     */
/* purely LOCAL, settable per-node property — it is NOT yet gossiped   */
/* over SWIM (that is a deferred slice). The self-test sets this table */
/* directly to simulate an already-converged membership+capability     */
/* view; production code sets only the self entry via PKERNEL_SUPERNODE.*/
static UB super_capable[DNODE_MAX]; /* 1 = node volunteered & reachable */
static BOOL super_init_done = FALSE;

/* ------------------------------------------------------------------ */
/* Teacher capability (T-fix-a, thread-t-impl-plan.md §2.3)            */
/*                                                                     */
/* EXACT mirror of super_capable[]: 1 = node holds a successfully       */
/* lm_load'ed teacher GGUF (a verifiable runtime property, set from     */
/* SWIM gossip bit 1 verbatim by swim.c's gossip_apply, under the same  */
/* anti-stale (incarnation,state) LWW gate as the supernode bit). This  */
/* table is the SELECTION input only; teacher truth originates in       */
/* swim.c::teacher_self() (gated on a real GGUF, never a bare env). */
static UB teacher_capable[DNODE_MAX]; /* 1 = node self-declares teacher-capable */

/* ------------------------------------------------------------------ */
/* 再計算                                                              */
/* ------------------------------------------------------------------ */

void region_recompute(void)
{
    for (UB n = 0; n < DNODE_MAX; n++) member[n] = 0;
    member_cnt = 0;
    coord      = 0xFF;

    UB me = drpc_my_node;
    if (me == 0xFF) return;   /* 分散モード未確立 — region 無し */

    /* 自分は常にメンバ */
    member[me] = 1;
    member_cnt = 1;
    coord      = me;

    /* ALIVE かつ RTT≤τ のノードを取り込む */
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == me) continue;
        if (dnode_table[n].state != DNODE_ALIVE) continue;
        UW rtt = swim_rtt_ms(n);
        if (rtt == 0xFFFFFFFFUL) continue;   /* 未実測は判定保留 */
        if (rtt <= REGION_TAU_MS) {
            member[n] = 1;
            member_cnt++;
            if (n < coord) coord = n;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Supernode selection (N-2 first slice — selection function only)     */
/*                                                                     */
/* p2p-overlay.md "Supernodes (N-2)": the per-region supernode is the  */
/* LOWEST-id node that is BOTH a current region member AND             */
/* supernode-capable, recomputed locally by every node → convergence   */
/* with NO vote/election, survives death by recomputation (the same    */
/* deterministic pure-function pattern as region_coordinator()). If NO  */
/* capable member exists, return 0xFF == "fall back to the central     */
/* relay" — the correct Skype-style graceful degrade.                  */
/*                                                                     */
/* DEFERRED (not in this slice): SWIM capability gossip, supernode      */
/* packet forwarding (relay REL_DATA/REL_BROADCAST relocation), and     */
/* NAT hole-punch. capability here is a LOCAL table only.              */
/* ------------------------------------------------------------------ */

void region_set_super_capable(UB node, BOOL yes)
{
    if (node < DNODE_MAX) super_capable[node] = yes ? 1 : 0;
}

BOOL region_is_super_capable(UB node)
{
    return (node < DNODE_MAX && super_capable[node]) ? TRUE : FALSE;
}

/* Read the self's capability opt-in once. Default: NOT capable
 * (conservative — a node volunteers explicitly). On hosted nodes the
 * opt-in is the env PKERNEL_SUPERNODE=1; bare-metal has no env so the
 * self stays non-capable until set programmatically. */
void region_super_init(void)
{
    if (super_init_done) return;
    super_init_done = TRUE;
#ifdef _TK_HOSTED_LIBC_
    {
        extern char *getenv(const char *);
        const char *e = getenv("PKERNEL_SUPERNODE");
        if (e && e[0] == '1' && drpc_my_node != 0xFF)
            super_capable[drpc_my_node] = 1;
    }
#endif
}

/* Pure core: lowest id that is BOTH a member (mbr[id]) AND capable
 * (cap[id]); 0xFF if none. Deterministic, allocation-free, O(N),
 * integer-only (reads no float). Same result on every arch / node. */
static UB supernode_select(const UB *mbr, const UB *cap)
{
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (mbr[n] && cap[n]) return n;   /* lowest id wins */
    }
    return 0xFF;                          /* no capable member → relay */
}

/* The per-region supernode for the self's current local view. Recomputes
 * membership (same source of truth as region_coordinator()), then picks
 * the lowest capable member. 0xFF = no capable member → central relay. */
UB region_supernode(void)
{
    region_super_init();
    region_recompute();
    return supernode_select(member, super_capable);
}

/* ------------------------------------------------------------------ */
/* Teacher selection (T-fix-a — thread-t-impl-plan.md §2.3)            */
/*                                                                     */
/* EXACT mirror of supernode_select()/region_supernode(): the per-region */
/* teacher is the LOWEST-id node that is BOTH a current region member    */
/* AND teacher-capable. Recomputed locally by every node → convergence   */
/* with NO vote/election (NOCENTRAL), survives the teacher's death by     */
/* recomputation (kill the teacher → member[]=0 → next teacher-capable    */
/* id wins). 0xFF = no teacher-capable member → degrade (the child keeps   */
/* its lesson ring / falls back to the committed TEACHER_FIXTURE).        */
/* ------------------------------------------------------------------ */

void region_set_teacher_capable(UB node, BOOL yes)
{
    if (node < DNODE_MAX) teacher_capable[node] = yes ? 1 : 0;
}

BOOL region_is_teacher_capable(UB node)
{
    return (node < DNODE_MAX && teacher_capable[node]) ? TRUE : FALSE;
}

/* Pure core: lowest id that is BOTH a member (mbr[id]) AND teacher-capable
 * (tch[id]); 0xFF if none. Deterministic, allocation-free, O(N), integer-only
 * (reads no float). Same result on every arch / node — the byte-identical
 * twin of supernode_select(). */
static UB teacher_select(const UB *mbr, const UB *tch)
{
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (mbr[n] && tch[n]) return n;   /* lowest id wins */
    }
    return 0xFF;                          /* no teacher member → degrade */
}

/* The per-region teacher for the self's current local view. Recomputes
 * membership (same source of truth as region_coordinator()/region_supernode()),
 * then picks the lowest teacher-capable member. 0xFF = none → degrade. */
UB region_teacher(void)
{
    region_recompute();
    return teacher_select(member, teacher_capable);
}

/* ------------------------------------------------------------------ */
/* 問い合わせ API                                                      */
/* ------------------------------------------------------------------ */

UB region_id(void)
{
    region_recompute();
    return coord;
}

UB region_coordinator(void)
{
    region_recompute();
    return coord;
}

/* 最後の region_recompute() の結果を読む (再計算しない; ホットパス用)。
 * kdds_pub のような per-packet ループは region_recompute() を一度呼んでから
 * これでメンバ判定すると O(N²) を避けられる。 */
BOOL region_is_member(UB node)
{
    return (node < DNODE_MAX && member[node]) ? TRUE : FALSE;
}

BOOL region_contains(UB node)
{
    region_recompute();
    return region_is_member(node);
}

UB region_size(void)
{
    region_recompute();
    return member_cnt;
}

/* ------------------------------------------------------------------ */
/* 表示                                                                */
/* ------------------------------------------------------------------ */

void region_print(void)
{
    region_recompute();

    if (drpc_my_node == 0xFF) {
        rg_puts("[region] single-node (no cluster)\r\n");
        return;
    }

    rg_puts("[region] id="); rg_putdec(coord);
    rg_puts("  coordinator=node"); rg_putdec(coord);
    if (coord == drpc_my_node) rg_puts(" (self)");
    rg_puts("  size="); rg_putdec(member_cnt);
    rg_puts("  tau="); rg_putdec(REGION_TAU_MS); rg_puts("ms\r\n");

    rg_puts("[region] members:");
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (!member[n]) continue;
        rg_puts(" node"); rg_putdec(n);
        if (n == drpc_my_node) {
            rg_puts("(self)");
        } else {
            UW rtt = swim_rtt_ms(n);
            rg_puts("(rtt="); rg_putdec(rtt); rg_puts("ms)");
        }
    }
    rg_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* Supernode self-test (host cert, N-2 first slice)                    */
/*                                                                     */
/* Proves the selection function against a SYNTHETIC converged view    */
/* (member[] + super_capable[]), independent of live RTT — so it is    */
/* deterministic and arch-uniform. NO vote/election is called          */
/* anywhere; selection is pure recomputation.                          */
/* ------------------------------------------------------------------ */

static INT super_fail;

static void super_check(BOOL cond, const char *name)
{
    if (cond) { rg_puts("[region-super]   PASS "); }
    else      { rg_puts("[region-super]   FAIL "); super_fail++; }
    rg_puts(name); rg_puts("\r\n");
}

void region_supernode_test(void)
{
    super_fail = 0;
    rg_puts("[region-super] supernode selection cert (N-2 slice 1)\r\n");

    /* Synthetic converged view A:
     *   members  = {1,2,5,7}        (node 1 present)
     *   capable  = {2,5,7}          (node 1 present but NOT capable)
     * Expected supernode = 2 (lowest CAPABLE member, not lowest member). */
    UB mbr[DNODE_MAX]; UB cap[DNODE_MAX];
    for (UB i = 0; i < DNODE_MAX; i++) { mbr[i] = 0; cap[i] = 0; }
    mbr[1] = 1; mbr[2] = 1; mbr[5] = 1; mbr[7] = 1;
    cap[2] = 1; cap[5] = 1; cap[7] = 1;

    /* (1) lowest-CAPABLE wins — node 1 is a member but incapable → 2. */
    UB sn = supernode_select(mbr, cap);
    super_check(sn == 2, "lowest-capable wins (member 1 incapable -> 2)");

    /* (2) convergence / determinism: the selector is a PURE function of
     * (mbr,cap) — it does not read the self id. Computing it "as" several
     * different self-ids over the SAME view yields the IDENTICAL answer.
     * (We assert purity by invoking it repeatedly; same inputs, same out.) */
    UB c0 = supernode_select(mbr, cap);
    UB c1 = supernode_select(mbr, cap);
    UB c3 = supernode_select(mbr, cap);
    UB c9 = supernode_select(mbr, cap);
    super_check(c0 == sn && c1 == sn && c3 == sn && c9 == sn,
                "convergence: every node computes the same id, no vote");

    /* (3) survives death: mark the current supernode (node 2) DEAD in the
     * membership view, recompute → next-lowest CAPABLE member (5) is
     * selected, with NO election call. */
    mbr[2] = 0;   /* node 2 left/died — drops out of membership */
    UB sn2 = supernode_select(mbr, cap);
    super_check(sn2 == 5, "survives death: supernode 2 dead -> 5 by recompute");
    mbr[2] = 1;   /* restore */

    /* (4) survives a chain of deaths: kill 2 and 5 → 7. */
    mbr[2] = 0; mbr[5] = 0;
    super_check(supernode_select(mbr, cap) == 7,
                "survives chained death: 2,5 dead -> 7");
    mbr[2] = 1; mbr[5] = 1;

    /* (5) relay fallback: ZERO capable members → 0xFF (degrade to relay). */
    for (UB i = 0; i < DNODE_MAX; i++) cap[i] = 0;   /* nobody volunteered */
    super_check(supernode_select(mbr, cap) == 0xFF,
                "relay fallback: no capable member -> 0xFF");

    /* (6) capable-but-not-member is ignored: capable {3}, member {5,7}.
     * Supernode must be a MEMBER of the region, so 5 (not 3). */
    for (UB i = 0; i < DNODE_MAX; i++) { mbr[i] = 0; cap[i] = 0; }
    mbr[5] = 1; mbr[7] = 1; cap[3] = 1; cap[5] = 1; cap[7] = 1;
    super_check(supernode_select(mbr, cap) == 5,
                "non-member capable ignored: capable 3 not in region -> 5");

    /* (7) the public setter feeds the real table consistently. */
    region_set_super_capable(11, TRUE);
    region_set_super_capable(11, FALSE);
    super_check(region_is_super_capable(11) == FALSE,
                "setter toggles capability table");
    region_set_super_capable(DNODE_MAX + 5, TRUE);  /* out of range = no-op */
    super_check(region_is_super_capable(DNODE_MAX + 5) == FALSE,
                "setter bounds-checks out-of-range node");

    rg_puts("[region-super] ");
    rg_putdec((UW)(8 - super_fail));
    rg_puts(" PASS, ");
    rg_putdec((UW)super_fail);
    rg_puts(" FAIL\r\n");

    /* ---------------------------------------------------------------- *
     * T-fix-a: teacher_select() is the byte-identical twin of           *
     * supernode_select(); a focused mirror cert so `region test` also   *
     * gates the teacher selector (deterministic / lowest-id-member /    *
     * survives-death / degrade). Kept on a SEPARATE counter+line so the *
     * supernode "[region-super] 8/8" headline does not regress.         *
     * ---------------------------------------------------------------- */
    {
        INT tch_fail = 0;
        UB  m2[DNODE_MAX], t2[DNODE_MAX];
        for (UB i = 0; i < DNODE_MAX; i++) { m2[i] = 0; t2[i] = 0; }
        /* members {1,2,5,7}; teacher-capable {2,5,7} (1 is a member but not a
         * teacher). Expected teacher = 2 (lowest CAPABLE member, not member 1). */
        m2[1] = 1; m2[2] = 1; m2[5] = 1; m2[7] = 1;
        t2[2] = 1; t2[5] = 1; t2[7] = 1;

        UB tn = teacher_select(m2, t2);
        if (tn != 2) tch_fail++;
        rg_puts(tn == 2 ? "[region-teacher]   PASS " : "[region-teacher]   FAIL ");
        rg_puts("lowest-capable wins (member 1 not a teacher -> 2)\r\n");

        /* determinism: pure fn of (mbr,tch); repeated calls give the SAME id. */
        UB d0 = teacher_select(m2, t2), d1 = teacher_select(m2, t2),
           d2 = teacher_select(m2, t2);
        if (!(d0 == tn && d1 == tn && d2 == tn)) tch_fail++;
        rg_puts((d0 == tn && d1 == tn && d2 == tn)
                ? "[region-teacher]   PASS " : "[region-teacher]   FAIL ");
        rg_puts("determinism: every node computes the same teacher, no vote\r\n");

        /* survives death: teacher (2) leaves membership -> next teacher (5). */
        m2[2] = 0;
        UB tn2 = teacher_select(m2, t2);
        if (tn2 != 5) tch_fail++;
        rg_puts(tn2 == 5 ? "[region-teacher]   PASS " : "[region-teacher]   FAIL ");
        rg_puts("survives death: teacher 2 dead -> 5 by recompute\r\n");
        m2[2] = 1;

        /* degrade: ZERO teacher-capable members -> 0xFF (no teacher region). */
        for (UB i = 0; i < DNODE_MAX; i++) t2[i] = 0;
        UB tnf = teacher_select(m2, t2);
        if (tnf != 0xFF) tch_fail++;
        rg_puts(tnf == 0xFF ? "[region-teacher]   PASS " : "[region-teacher]   FAIL ");
        rg_puts("degrade: no teacher-capable member -> 0xFF\r\n");

        /* the public setter feeds the real table; bounds-checked. */
        region_set_teacher_capable(11, TRUE);
        region_set_teacher_capable(11, FALSE);
        if (region_is_teacher_capable(11) != FALSE) tch_fail++;
        rg_puts(region_is_teacher_capable(11) == FALSE
                ? "[region-teacher]   PASS " : "[region-teacher]   FAIL ");
        rg_puts("setter toggles teacher table\r\n");
        region_set_teacher_capable(DNODE_MAX + 5, TRUE);  /* out of range no-op */
        if (region_is_teacher_capable(DNODE_MAX + 5) != FALSE) tch_fail++;
        rg_puts(region_is_teacher_capable(DNODE_MAX + 5) == FALSE
                ? "[region-teacher]   PASS " : "[region-teacher]   FAIL ");
        rg_puts("setter bounds-checks out-of-range node\r\n");

        rg_puts("[region-teacher] ");
        rg_putdec((UW)(6 - tch_fail));
        rg_puts(" PASS, ");
        rg_putdec((UW)tch_fail);
        rg_puts(" FAIL\r\n");
    }
}
