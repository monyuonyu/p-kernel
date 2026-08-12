---
name: thinkpad-reboot-db-shutdown-order
description: thinkpadサーバは、DBコンテナのシャットダウン完了前に絶対rebootしない（過去にDB破損）。
metadata: 
  node_type: memory
  type: feedback
  originSessionId: a3e4865c-2a33-4c93-8048-8162925105b6
---

自宅サーバ `shota-ThinkPad-X1-Carbon-5th`（LAN 192.168.10.100 / 外部 helloidea.org:2222、sudo `echo PASS|sudo -S`）を再起動する時は、**DBコンテナを先に手動でgraceful停止し、完全停止を確認してから `reboot`** する。

**Why:** 過去にPostgresのシャットダウン完了前にホストが再起動し、**データベースが壊れた**実体験がある（mk_pino談 2026-07-03）。`immich_postgres` は `stop_grace_period=300s` なのに systemd の `DefaultTimeoutStopSec`（既定~90s）が先にSIGKILLし得る＝これが破損の機序と推定。

**How to apply:**
1. `docker stop immich_postgres guacamole_postgres nextcloud系DB` を明示実行し、`docker ps -a` で Exited を確認してから reboot。
2. `apt upgrade` で `docker-ce`/`containerd.io` を更新するとデーモン再起動でコンテナも落ちるので、同様にDBを先に落としてから行う。
3. できれば再起動は本人が在宅・目の届く時に。

関連: [[project_ump_android_node]] のrelayや gh-runner も同ホスト上。
