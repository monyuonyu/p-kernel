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
# --- MEM-UPTR（層B）: LLP64 でのポインタ幅マスク。f50c30a0 に焼き込まれ ff163ac1 が拡張。
check MEM-UPTR  memory.h     10 'KNL_UPTR'

echo
echo "pass=$pass lost=$fail"
if [ "$fail" -ne 0 ]; then
    echo "RESULT: RED — ベンダツリー上のローカルパッチが失われている"
    exit 1
fi
echo "RESULT: GREEN"
exit 0
