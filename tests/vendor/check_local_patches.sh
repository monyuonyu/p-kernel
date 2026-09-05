#!/bin/sh
# check_local_patches.sh — ベンダツリー kernel/mtkernel3/ に載っている
# 「静かに消える」ローカルパッチが、まだそこに在ることを確認する。
#
# 使い方:  sh check_local_patches.sh <repo-root>
#
# 由来: gap-ledger 行 VENDOR-PATCH-LOSS。
#   2026-07-02 の `f50c30a0`（μT-Kernel 2.0→3.0）が L1–L7 のハードニングを削除し、
#   ビルドは通り続けたため 41 日間、台帳が CURED と嘘をつき続けた。
#   ビルドが落ちる種類の消失（新規 sysdepend ディレクトリ等）は既存ジョブが捕まえるので
#   ここでは扱わない。**扱うのは「ビルドは通るのに挙動だけ壊れる」クラスだけ。**
#
# 判定は「アンカー文字列の出現回数 >= 最低数」。等号ではないのは、
# 将来の増加（同種のガードの追加）で赤くしないため。

set -u

ROOT="${1:-.}"
K="$ROOT/kernel/mtkernel3/kernel/tkernel"

fail=0
pass=0

# check <id> <file> <min> <anchor...>
check() {
    id="$1"; shift
    f="$1"; shift
    min="$1"; shift
    anchor="$1"

    path="$K/$f"
    if [ ! -f "$path" ]; then
        printf '%-10s %-14s MISSING-FILE          need>=%s  anchor=[%s]\n' "$id" "$f" "$min" "$anchor"
        fail=$((fail + 1))
        return
    fi
    n=$(grep -c -F -- "$anchor" "$path")
    if [ "$n" -ge "$min" ]; then
        printf '%-10s %-14s OK    found=%-3s need>=%s  anchor=[%s]\n' "$id" "$f" "$n" "$min" "$anchor"
        pass=$((pass + 1))
    else
        printf '%-10s %-14s LOST  found=%-3s need>=%s  anchor=[%s]\n' "$id" "$f" "$n" "$min" "$anchor"
        fail=$((fail + 1))
    fi
}

echo "== local-patch inventory check =="
echo "root: $ROOT"
echo

# --- L1: wait-timer が「もう時限待ちでない TCB」に発火したときのガード（元 2dbacd66）
#     count は 2。L2 の KCC_DIAG 側が同じ条件を鏡写しにしているため。
#     したがって判定は「1 なら赤」ではなく「2 未満なら赤」。
check L1        wait.c        2 'TS_WAIT) == 0'
# --- L2: KCC_DIAG 計装（元 d63ff19c）
check L2        wait.c        2 'KCC_DIAG'
# --- L3: QueRemove の後に QueInit してぶら下がりを断つ（元 f109a3c4）
check L3a       wait.c        1 'QueInit(&tcb->tskque)'
check L3b       wait.h        1 'QueInit(&tcb->tskque)'
check L3c       timer.c       2 'QueInit(&event->queue)'
check L3d       timer.h       1 'QueInit(&event->queue)'
# --- L4: キュー走査のループ上限（元 f109a3c4）
#     注意: 部分一致に注意。'cnt > 1000' と書くと 'cnt > 10000' にも当たる。
#     必ず下の形（桁まで含めた固定文字列）で区別すること。
check L4a       timer.c       1 'cnt > 10000'
check L4b       timer.c       1 'timer_loop_cnt > 1000'
# --- L5: TCB の wtmeb 衛生（元 8924e8d3）
check L5a       task.c        1 'QueInit(&tcb->wtmeb.queue)'
check L5b       task.c        1 'knl_timer_delete(&tcb->wtmeb)'
# --- L6: knl_del_tsk でのタイマ解除（元 8924e8d3）
check L6        task_manage.c 1 'knl_timer_delete(&tcb->wtmeb)'
# --- L7: tk_del_tsk の ctxtsk/schedtsk ガード（元 e4e5d9d6）
check L7        task_manage.c 1 'tcb == knl_ctxtsk || tcb == knl_schedtsk'
# --- MEM-UPTR: LLP64 でのポインタ幅マスク。訂正(vendor-patch-inventory.md §3):
#     f50c30a0 の memory.h に KNL_UPTR は 0 個で、正しくは ff163ac1（層A）が導入した。
#     memory.c / mempool.c にも同じ保護があるが、従来 memory.h しか見ていなかった
#     （どちらか片方だけ差し戻されても見逃す穴だった。下の2行で埋める）。
check MEM-UPTR     memory.h     10 'KNL_UPTR'
check MEM-UPTR-C   memory.c      1 'KNL_UPTR'
check MEM-UPTR-MP  mempool.c     1 'KNL_UPTR'
# --- B-SCHED（層B、2026-09-06 発見）: 同一優先度内ラウンドロビン(SCHED_RR)。
#     kernel.h のコメントに「micro T-Kernel 2.0 ポートから移植」とあり、2.0→3.0 移行
#     (f50c30a0) 自身に焼き込まれた真の層Bパッチ。上流 435096c9 には該当アンカーが
#     いずれも0個であることを実測済み（vendor-patch-inventory.md §7）。
check B-SCHED-KH   ../knlinc/kernel.h    1 'DEFAULT_TIME_SLICE'
check B-SCHED-TM   task_manage.c         2 'DEFAULT_TIME_SLICE'
check B-SCHED-TC   timer.c               1 'knl_ctxtsk->sched_policy == SCHED_RR'
# --- B-STR（層B、2026-09-06 発見）: tstdlib/string.c の語コピーを sizeof(unsigned long) 基準に
#     統一した LP64 修正。上流は 4 バイト固定のまま（実測で確認）。min=4 はコード側4箇所のみを
#     数え、コメント文言の書き換え（この行を含めると計5）だけでは赤くならないようにしている。
check B-STR        ../tstdlib/string.c   4 'sizeof(unsigned long)'

echo
echo "pass=$pass lost=$fail"
if [ "$fail" -ne 0 ]; then
    echo "RESULT: RED — ベンダツリー上のローカルパッチが失われている"
    exit 1
fi
echo "RESULT: GREEN"
exit 0
