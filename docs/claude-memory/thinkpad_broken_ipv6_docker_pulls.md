---
name: thinkpad-broken-ipv6-docker-pulls
description: thinkpadサーバはIPv6が壊れて(存在せず)大きいdocker pullがストールする。一時無効化で回避。
metadata: 
  node_type: memory
  type: reference
  originSessionId: a3e4865c-2a33-4c93-8048-8162925105b6
---

自宅サーバ `shota-ThinkPad-X1-Carbon-5th`（helloidea.org:2222 / LAN 192.168.10.100）は **グローバルIPv6アドレスもIPv6デフォルトルートも持たない**（`ip -6 addr`空・`curl -6`全滅)。しかしDNSはAAAAを返すため、既定のIPv6優先で **大きいdocker pullが0B/途中でストール**する（小さいイメージは通ることも）。

**回避策（2026-07-03に実施・mk_pino承認済み）:**
1. `sysctl -w net.ipv6.conf.all.disable_ipv6=1`（+default）でIPv6一時無効 → pullがIPv4に即フォールバックして完走。**pull後は必ず `=0` に戻す**（コンテナ再作成時に`[::]`バインドが要るため）。
2. `/etc/gai.conf` に `precedence ::ffff:0:0/96 100`（IPv4優先）を追加済み。ただし **dockerデーモンはGo製リゾルバでgai.confを無視**するので、pull対策には sysctl 無効化が要る。
3. 大きいpullは `setsid nohup` でデタッチ＋`timeout 3600`＋リトライ。SSH切断でもリモートで生存し完走する。

恒久対策候補: IPv6を`/etc/sysctl.d/`で永続無効化（要本人判断・ネットワークスタック変更なのでauto-modeがブロックする）。関連: [[thinkpad-reboot-db-shutdown-order]]
