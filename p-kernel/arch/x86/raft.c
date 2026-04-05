/*
 *  raft.c (x86)
 *  Phase 10 — Raft コンセンサスアルゴリズム
 *
 *  リーダー選出 + ハートビート + 簡易ログレプリケーション。
 *
 *  drpc.c の dnode_table を参照して生存ノードを把握する。
 *  過半数 = ceil((alive_count + 1) / 2)
 *
 *  Raft ルール:
 *    1. term > 自分の term のメッセージを受けたら即 FOLLOWER に降格
 *    2. election timeout (ノードIDでずらす) で CANDIDATE 昇格
 *    3. 同一 term に 1 票のみ投票する (voted_for)
 *    4. LEADER は HB_INTERVAL ごとに AppendEntries(heartbeat)を送信
 *    5. 生存ノードの過半数から VOTE_REP(granted=1) を得たら LEADER
 */

#include "raft.h"
#include "drpc.h"
#include "netstack.h"
#include "kernel.h"
#include "spawn.h"

IMPORT void sio_send_frame(const UB *buf, INT size);

/* ------------------------------------------------------------------ */
/* 出力ヘルパー                                                        */
/* ------------------------------------------------------------------ */

static void rf_puts(const char *s)
{
    INT n = 0; while (s[n]) n++;
    sio_send_frame((const UB *)s, n);
}

static void rf_putdec(UW v)
{
    char buf[12]; INT i = 11; buf[i] = '\0';
    if (v == 0) { rf_puts("0"); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    rf_puts(&buf[i]);
}

/* ------------------------------------------------------------------ */
/* モジュール状態                                                      */
/* ------------------------------------------------------------------ */

static UB   role       = RAFT_FOLLOWER;
static UW   cur_term   = 0;
static UB   voted_for  = 0xFF;   /* 現任期で投票したノードID */
static UB   cur_leader = 0xFF;   /* 現在のリーダー          */
static UW   votes_got  = 0;      /* 現任期で得た票数        */

/* Election timeout カウンタ (tick 単位) */
static INT  elect_tmo  = 0;
static INT  elect_tmo_max = RAFT_ELECT_TMO_MIN;  /* 初期化時に設定 */

/* ログ */
static RAFT_LOG_ENTRY log_buf[RAFT_LOG_MAX];
static UW             log_size    = 0;
static UW             commit_idx  = 0;

/* ------------------------------------------------------------------ */
/* ヘルパー                                                            */
/* ------------------------------------------------------------------ */

static UW node_ip(UB n)
{
    /* drpc.c と同じアドレッシング: 10.1.0.(n+1) */
    return ((UW)(n + 1) << 24) | 0x0000010AUL;
}

/* 生存ノード数 (自分を含む) を返す */
static UW alive_count(void)
{
    UW cnt = 1;
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == drpc_my_node) continue;
        if (dnode_table[n].state == DNODE_ALIVE) cnt++;
    }
    return cnt;
}

/* 過半数を返す */
static UW quorum(void)
{
    return alive_count() / 2 + 1;
}

/* FOLLOWER に戻る */
static void become_follower(UW new_term)
{
    if (new_term > cur_term) {
        cur_term  = new_term;
        voted_for = 0xFF;
    }
    role      = RAFT_FOLLOWER;
    elect_tmo = 0;
}

/* ------------------------------------------------------------------ */
/* パケット送信                                                        */
/* ------------------------------------------------------------------ */

static void send_pkt(UW dst_ip, UB type, UB extra_grant, UB extra_success)
{
    RAFT_PKT p = { 0 };
    p.magic      = RAFT_MAGIC;
    p.version    = RAFT_VERSION;
    p.type       = type;
    p.src_node   = drpc_my_node;
    p.candidate  = drpc_my_node;
    p.term       = cur_term;
    p.log_idx    = log_size;
    p.log_term   = (log_size > 0) ? log_buf[log_size - 1].term : 0;
    p.commit_idx = commit_idx;
    p.granted    = extra_grant;
    p.success    = extra_success;

    /* ログエントリ (AppendEntries 用: 最新エントリを添付) */
    if (type == RAFT_MSG_APPEND && log_size > 0) {
        p.entry_idx  = log_size - 1;
        p.entry_term = log_buf[log_size - 1].term;
        p.entry_key  = log_buf[log_size - 1].key;
        p.entry_val  = log_buf[log_size - 1].val;
    }

    udp_send(dst_ip, RAFT_PORT, RAFT_PORT, (const UB *)&p, (UH)sizeof(p));
}

static void broadcast(UB type)
{
    for (UB n = 0; n < DNODE_MAX; n++) {
        if (n == drpc_my_node) continue;
        if (dnode_table[n].state == DNODE_ALIVE ||
            dnode_table[n].state == DNODE_UNKNOWN) {
            send_pkt(node_ip(n), type, 0, 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* ログ適用                                                            */
/* ------------------------------------------------------------------ */

static void apply_entry(UW idx)
{
    if (idx >= log_size) return;
    /* 現時点では key/val をシリアルに表示するのみ (将来は設定に反映) */
    rf_puts("[raft] apply  idx="); rf_putdec(idx);
    rf_puts("  key="); rf_putdec(log_buf[idx].key);
    rf_puts("  val="); rf_putdec(log_buf[idx].val);
    rf_puts("\r\n");
}

/* ------------------------------------------------------------------ */
/* UDP 受信コールバック                                                */
/* ------------------------------------------------------------------ */

void raft_rx(UW src_ip, UH src_port, const UB *data, UH len)
{
    (void)src_ip; (void)src_port;
    if (len < (UH)sizeof(RAFT_PKT)) return;

    const RAFT_PKT *p = (const RAFT_PKT *)data;
    if (p->magic != RAFT_MAGIC || p->version != RAFT_VERSION) return;

    /* 古い任期のメッセージは無視 */
    if (p->term < cur_term) return;

    /* 新しい任期を発見したら即 FOLLOWER */
    if (p->term > cur_term) {
        rf_puts("[raft] newer term="); rf_putdec(p->term);
        rf_puts(" from node "); rf_putdec(p->src_node);
        rf_puts(" -> FOLLOWER\r\n");
        become_follower(p->term);
    }

    switch (p->type) {

    /* ---- RequestVote ---- */
    case RAFT_MSG_VOTE_REQ: {
        UB grant = 0;

        /* 投票条件: まだ投票していない (or 同じcandidateに再投票) */
        if (voted_for == 0xFF || voted_for == p->candidate) {
            /* candidateのログが自分以上に新しいか確認 */
            UW my_log_term = (log_size > 0) ? log_buf[log_size - 1].term : 0;
            INT log_ok = (p->log_term > my_log_term) ||
                         (p->log_term == my_log_term && p->log_idx >= log_size);
            if (log_ok) {
                grant     = 1;
                voted_for = p->candidate;
                elect_tmo = 0;   /* 投票したら timeout リセット */
            }
        }

        /* 返答 */
        RAFT_PKT rep = { 0 };
        rep.magic    = RAFT_MAGIC;
        rep.version  = RAFT_VERSION;
        rep.type     = RAFT_MSG_VOTE_REP;
        rep.src_node = drpc_my_node;
        rep.term     = cur_term;
        rep.granted  = grant;
        udp_send(node_ip(p->candidate), RAFT_PORT, RAFT_PORT,
                 (const UB *)&rep, (UH)sizeof(rep));
        break;
    }

    /* ---- RequestVote 返答 ---- */
    case RAFT_MSG_VOTE_REP: {
        if (role != RAFT_CANDIDATE) break;
        if (p->term != cur_term) break;
        if (!p->granted) break;

        votes_got++;
        rf_puts("[raft] vote from node "); rf_putdec(p->src_node);
        rf_puts("  total="); rf_putdec(votes_got);
        rf_puts("/"); rf_putdec(quorum()); rf_puts("\r\n");

        if (votes_got >= quorum()) {
            role       = RAFT_LEADER;
            cur_leader = drpc_my_node;
            rf_puts("[raft] *** LEADER elected  term=");
            rf_putdec(cur_term); rf_puts("\r\n");
            /* 即時 heartbeat 送信 */
            broadcast(RAFT_MSG_APPEND);
            /* 自己増殖: 新規ノードへカーネル配布 */
            spawn_on_leader();
        }
        break;
    }

    /* ---- AppendEntries (heartbeat + log) ---- */
    case RAFT_MSG_APPEND: {
        /* リーダーからの heartbeat でタイムアウトリセット */
        cur_leader = p->src_node;
        elect_tmo  = 0;

        if (role == RAFT_CANDIDATE) {
            rf_puts("[raft] lost election, new leader=");
            rf_putdec(p->src_node); rf_puts("\r\n");
            become_follower(p->term);
        }

        /* ログ複製: 新しいエントリがあれば追加 */
        if (p->log_idx < RAFT_LOG_MAX && p->entry_term > 0) {
            if (p->entry_idx >= log_size) {
                log_buf[log_size].term = p->entry_term;
                log_buf[log_size].key  = p->entry_key;
                log_buf[log_size].val  = p->entry_val;
                log_size++;
            }
        }

        /* コミット済みエントリを適用 */
        while (commit_idx < p->commit_idx && commit_idx < log_size) {
            apply_entry(commit_idx);
            commit_idx++;
        }

        /* 返答 */
        RAFT_PKT rep = { 0 };
        rep.magic    = RAFT_MAGIC;
        rep.version  = RAFT_VERSION;
        rep.type     = RAFT_MSG_APPEND_REP;
        rep.src_node = drpc_my_node;
        rep.term     = cur_term;
        rep.log_idx  = log_size;
        rep.success  = 1;
        udp_send(node_ip(p->src_node), RAFT_PORT, RAFT_PORT,
                 (const UB *)&rep, (UH)sizeof(rep));
        break;
    }

    /* ---- AppendEntries 返答 ---- */
    case RAFT_MSG_APPEND_REP: {
        if (role != RAFT_LEADER) break;
        /* 将来: フォロワーのlog_idxを追跡してリトライ */
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Raft タスク                                                         */
/* ------------------------------------------------------------------ */

void raft_task(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;

    INT hb_counter = 0;

    for (;;) {
        tk_dly_tsk(RAFT_TICK_MS);

        if (drpc_my_node == 0xFF) continue;   /* 未初期化 */

        /* ---- LEADER: heartbeat 送信 ---- */
        if (role == RAFT_LEADER) {
            hb_counter++;
            if (hb_counter * RAFT_TICK_MS >= RAFT_HB_INTERVAL_MS) {
                hb_counter = 0;
                broadcast(RAFT_MSG_APPEND);
            }
            continue;
        }

        /* ---- FOLLOWER / CANDIDATE: election timeout ---- */
        elect_tmo++;
        if (elect_tmo < elect_tmo_max) continue;

        /* タイムアウト発生 → 選挙開始 */
        elect_tmo = 0;
        cur_term++;
        role      = RAFT_CANDIDATE;
        voted_for = drpc_my_node;   /* 自分に投票 */
        votes_got = 1;              /* 自票 */

        rf_puts("[raft] ** election  term="); rf_putdec(cur_term);
        rf_puts("  node="); rf_putdec(drpc_my_node); rf_puts("\r\n");

        /* 過半数チェック (自分だけなら即リーダー) */
        if (votes_got >= quorum()) {
            role       = RAFT_LEADER;
            cur_leader = drpc_my_node;
            rf_puts("[raft] LEADER (solo)  term="); rf_putdec(cur_term); rf_puts("\r\n");
            spawn_on_leader();
            continue;
        }

        /* 全生存ノードに RequestVote 送信 */
        broadcast(RAFT_MSG_VOTE_REQ);
    }
}

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void raft_init(void)
{
    role       = RAFT_FOLLOWER;
    cur_term   = 0;
    voted_for  = 0xFF;
    cur_leader = 0xFF;
    votes_got  = 0;
    elect_tmo  = 0;
    log_size   = 0;
    commit_idx = 0;

    /* ノードIDでタイムアウトをずらす (選挙の衝突を防ぐ) */
    /* node 0: 300ms, node 1: 350ms, ... */
    elect_tmo_max = RAFT_ELECT_TMO_MIN + (INT)drpc_my_node;

    udp_bind(RAFT_PORT, raft_rx);
    rf_puts("[raft] initialized  port="); rf_putdec(RAFT_PORT);
    rf_puts("  elect_tmo="); rf_putdec((UW)elect_tmo_max * RAFT_TICK_MS);
    rf_puts("ms\r\n");
}

/* ------------------------------------------------------------------ */
/* ログ書き込み (リーダーのみ)                                        */
/* ------------------------------------------------------------------ */

ER raft_write(UB key, UB val)
{
    if (role != RAFT_LEADER) {
        rf_puts("[raft] write denied: not leader\r\n");
        return E_RSFN;
    }
    if (log_size >= RAFT_LOG_MAX) {
        rf_puts("[raft] log full\r\n");
        return E_LIMIT;
    }
    log_buf[log_size].term = cur_term;
    log_buf[log_size].key  = key;
    log_buf[log_size].val  = val;
    log_size++;

    /* 直ちに broadcast して followers に複製 */
    broadcast(RAFT_MSG_APPEND);

    /* 過半数が確認したとみなしてコミット (簡易: 即コミット) */
    commit_idx = log_size;
    return E_OK;
}

/* ------------------------------------------------------------------ */
/* ゲッター                                                            */
/* ------------------------------------------------------------------ */

UB raft_leader(void) { return cur_leader; }
UB raft_role(void)   { return role; }
UW raft_term(void)   { return cur_term; }

/* ------------------------------------------------------------------ */
/* 統計表示                                                            */
/* ------------------------------------------------------------------ */

void raft_stat(void)
{
    static const char *rname[] = { "FOLLOWER", "CANDIDATE", "LEADER" };
    rf_puts("[raft] role   : "); rf_puts(rname[role < 3 ? role : 0]); rf_puts("\r\n");
    rf_puts("[raft] term   : "); rf_putdec(cur_term); rf_puts("\r\n");
    rf_puts("[raft] leader : ");
    if (cur_leader == 0xFF) rf_puts("unknown");
    else rf_putdec(cur_leader);
    rf_puts("\r\n");
    rf_puts("[raft] log    : "); rf_putdec(log_size); rf_puts(" entries");
    rf_puts("  commit="); rf_putdec(commit_idx); rf_puts("\r\n");
    rf_puts("[raft] alive  : "); rf_putdec(alive_count()); rf_puts(" nodes");
    rf_puts("  quorum="); rf_putdec(quorum()); rf_puts("\r\n");
}
