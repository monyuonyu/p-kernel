#!/bin/bash
# b5w2 M round, separating the limits of b5-wire2.md §7 (a measurement, not a test).
#   two arms AT THE SAME TIME (this box throttles; no A/B across time):
#     fs: as built (every received block: tmp write + fsync + rename + dir fsync, and a .org)
#     nf: pfs_durable.c without its two fsync calls (a scratch copy of the tree, the binary is static)
#   every 5 s per node: block files on disk, CPU ticks (user, sys), give-up lines, sent/replicated lines
#   -> ① does a peer fetch stop at GL_ST_FETCH_MAXPASS (4096 passes) / ② is the late slowdown fsync
#      or CPU (table/file probing) / ③ how many hold-acks a round sends (= replicated lines)
# usage (in pkernel_audit_ss): bash /build/b5w2/m3.sh   (needs /build/b5w2/m3.bundle with master)
set -u
A=/build/b5w2; W=$A/m3; S=$A/m3.txt; : > $S
rm -rf $W; mkdir -p $W
git clone -q $A/m3.bundle $W/src && cd $W/src && git checkout -q master || { echo "clone FAIL" >> $S; exit 1; }
echo "== M3 $(git log --oneline -1 | cut -c1-9) $(date -Is)" >> $S
B=$W/src/boot/linux_x86_64
make -C $B > $W/build.log 2>&1 || { echo "BUILD FAIL" >> $S; exit 1; }
make -C $W/src/relay > /dev/null 2>&1
# the binary is static (no LD_PRELOAD): the nf arm is a copy of the tree whose pfs_durable.c has
# its two fsync calls (the file, the directory) removed. Measurement only, never committed.
cp -r $W/src $W/src-nf
sed -i 's/if (fsync(fd) != 0) { close(fd); unlink(tmp); return -1; }/(void)0;/; s/(void)fsync(dfd);/(void)0;/' $W/src-nf/arch/linux/pfs_durable.c
echo "nf: fsync calls left in pfs_durable.c: $(grep -c 'fsync(' $W/src-nf/arch/linux/pfs_durable.c) (fs: $(grep -c 'fsync(' $W/src/arch/linux/pfs_durable.c))" >> $S
make -C $W/src-nf/boot/linux_x86_64 > $W/build-nf.log 2>&1 || { echo "BUILD nf FAIL" >> $S; exit 1; }
BNF=$W/src-nf/boot/linux_x86_64
cmp -s $B/p-kernel $BNF/p-kernel && { echo "nf binary == fs binary (sed did nothing)" >> $S; exit 1; }
export PKERNEL_RELAY_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
unset PKERNEL_RELAY_HOST PKERNEL_RELAY_PORT PKERNEL_LAN

declare -A PID
start_arm() { # arm port binary
  local arm=$1 port=$2 bin=$3
  mkdir -p $W/$arm
  $W/src/relay/relay -p $port > $W/$arm/relay.log 2>&1 &
  PID[$arm-r]=$!
  sleep 1
  for k in 1 2 3 4 5; do
    mkfifo $W/$arm/f$k; mkdir -p $W/$arm/d$k
    env PKERNEL_NODE_ID=$k PKERNEL_AUTONET=1 PKERNEL_PFS_DIR=$W/$arm/d$k \
        PKERNEL_RELAY=127.0.0.1:$port PKERNEL_DEVICE_TIER=M \
        $bin < $W/$arm/f$k > $W/$arm/n$k.log 2>&1 &
    PID[$arm-$k]=$!
  done
}
start_arm fs 7611 $B/p-kernel
start_arm nf 7612 $BNF/p-kernel
# fifos: fs on fd 11-15, nf on fd 21-25
for k in 1 2 3 4 5; do eval "exec $((10 + k))<>$W/fs/f$k"; eval "exec $((20 + k))<>$W/nf/f$k"; done
send_all() { # text
  for k in 1 2 3 4 5; do printf '%s\n' "$1" >&$((10 + k)); printf '%s\n' "$1" >&$((20 + k)); done
}
count_logs() { # pattern -> how many of the 10 logs have it
  grep -al "$1" $W/fs/n*.log $W/nf/n*.log 2>/dev/null | wc -l
}
n=0; while [ $n -lt 360 ]; do [ "$(count_logs 'mind_merge_task up')" -ge 10 ] && break; sleep 0.5; n=$((n+1)); done
echo "up after $((n/2)) s" >> $S

# sampler
sampler() {
  local T=$W/samp.tsv
  echo -e "t\tarm\tnode\tfiles\tutime\tstime\tgaveup\tsent\trepl\tregion" > $T
  while [ -f $W/sampling ]; do
    local t=$(date +%s)
    for arm in fs nf; do
      for k in 1 2 3 4 5; do
        local p=${PID[$arm-$k]}
        local files=$(ls -f $W/$arm/d$k 2>/dev/null | grep -c '^[0-9a-f]\{64\}$')
        local st=$(cut -d' ' -f14,15 /proc/$p/stat 2>/dev/null | tr ' ' '\t')
        local lg=$W/$arm/n$k.log
        echo -e "$t\t$arm\t$k\t$files\t${st:-x\tx}\t$(grep -ac 'gave up' $lg)\t$(grep -ac 'sent block' $lg)\t$(grep -ac 'replicated block' $lg)\t$(grep -ac '\[baby\] region: gen 7' $lg)" >> $T
      done
      local rp=${PID[$arm-r]}
      echo -e "$t\t$arm\trelay\t-\t$(cut -d' ' -f14,15 /proc/$rp/stat | tr ' ' '\t')\t-\t-\t-\t-" >> $T
    done
    sleep 5
  done
}
touch $W/sampling
sampler &
SP=$!

sleep 15
for k in 1 2 3 4 5; do printf 'baby 1 0.00%s 32\n' $k >&$((10 + k)); printf 'baby 1 0.00%s 32\n' $k >&$((20 + k)); done
n=0; while [ $n -lt 900 ]; do [ "$(count_logs 'held-out loss (after)')" -ge 10 ] && break; sleep 0.5; n=$((n+1)); done
echo "trained after $((n/2)) s, $(date -Is)" >> $S
send_all 'baby region 7'
t0=$(date +%s); echo "publish start $t0" >> $S
n=0; while [ $n -lt 1200 ]; do [ "$(count_logs '\[baby\] region: gen 7')" -ge 10 ] && break; sleep 0.5; n=$((n+1)); done
echo "publish+first region lines after $(( $(date +%s) - t0 )) s" >> $S
sleep 10
send_all 'baby region 7'
t1=$(date +%s); echo "fetch start $t1" >> $S
# wait for the second region line on every node, at most M3_MAX s (default 1500: the budget is
# 1,350 s, research/b5-m-round.md §1, so a node not done by then already fails M1)
MAX=${M3_MAX:-1500}
while [ $(( $(date +%s) - t1 )) -lt $MAX ]; do
  c=0
  for arm in fs nf; do for k in 1 2 3 4 5; do [ "$(grep -ac '\[baby\] region: gen 7' $W/$arm/n$k.log)" -ge 2 ] && c=$((c+1)); done; done
  [ "$c" -ge 10 ] && break
  sleep 5
done
echo "stop after $(( $(date +%s) - t1 )) s (10 = all done: $c)" >> $S
rm -f $W/sampling; sleep 6
for arm in fs nf; do for k in 1 2 3 4 5; do
  lg=$W/$arm/n$k.log
  echo "$arm n$k: $(grep -a '\[baby\] region' $lg | tail -1 | sed 's/.*members/members/') | gaveup: $(grep -a 'gave up' $lg | tr '\n' ';') | SUSPECT $(grep -ac -- '-> SUSPECT' $lg) SELF $(grep -ac 'SELF-SUSPICION' $lg) repl $(grep -ac 'replicated block' $lg) sent $(grep -ac 'sent block' $lg)" >> $S
done; done
send_all 'exit'
sleep 2
kill -9 ${PID[@]} $SP 2>/dev/null
for arm in fs nf; do du -sh $W/$arm | tr '\t' ' ' >> $S; rm -rf $W/$arm/d1 $W/$arm/d2 $W/$arm/d3 $W/$arm/d4 $W/$arm/d5; done
rm -rf $W/src $W/src-nf
echo DONE $(date -Is) >> $S
