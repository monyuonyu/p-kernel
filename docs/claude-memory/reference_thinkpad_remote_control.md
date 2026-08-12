---
name: reference-thinkpad-remote-control
description: ThinkPad(192.168.10.100)で Claude Code リモートコントロールを systemd user service で常時稼働させた構成と罠
metadata: 
  node_type: memory
  type: reference
  originSessionId: bf3e2b5d-5bfa-4071-9c37-514badfe2308
  modified: 2026-08-08T11:00:39.559Z
---

2026-07-20、mk_pino の ThinkPad(`shota@192.168.10.100`)に Claude Code を公式ネイティブバイナリで新規インストールし(`curl -fsSL https://claude.ai/install.sh | bash -s stable` → `~/.local/bin/claude`, v2.1.205, Node不要)、**Remote Control を「常に有効」**にした。claude.ai/code または Claude アプリの Code タブに **"ThinkPad Always-On"** として online で出る(アカウント monyuonyu@gmail.com / Max)。作業ディレクトリは `/home/shota`。

**常時稼働の要**: systemd **user** service `~/.config/systemd/user/claude-remote.service`(`ExecStart=%h/.local/bin/claude remote-control --name "ThinkPad Always-On"`, `Restart=always`)+ `loginctl enable-linger shota`(sudo不要で通る)+ `systemctl --user enable --now`。linger でログイン無し・再起動後も起動。管理は `systemctl --user {status,restart,stop,disable} claude-remote` / `journalctl --user -u claude-remote`。非対話SSHでは先に `export XDG_RUNTIME_DIR=/run/user/$(id -u)`。

**Remote Control 自体**(公式機能, 2026-02導入): コード実行/ファイルはローカルのまま、会話トランスクリプトのみ TLS で Anthropic に同期、**inbound ポートを開かない**(外向きのみ=既存の SSH 堅牢化/fail2ban と非衝突)。claude.ai OAuth **必須**(APIキー不可)。

**ハマった罠3つ**:
1. オンボーディングで trust を「Yes」承認しても `~/.claude.json` の `projects['/home/shota'].hasTrustDialogAccepted` が `False` のまま保存され、remote-control が「Workspace not trusted」で起動拒否 → その値を直接 `True` に書く(バックアップ後)。
2. 非対話起動だと `Enable Remote Control? (y/n)` で詰まる → `~/.claude/settings.json` に `"enableRemoteControlByDefault": true`。
3. TTY が無いと TUI が毎秒再描画して journal を埋め **CPU ~18%** → `Environment=TERM=dumb` + `StandardOutput=null`(`StandardError=journal`)で定常 **CPU 0.0%**。`--ax-screen-reader` は remote-control サブコマンドでは Unknown argument(top-level専用)。

**2026-08-08: cross-session messaging(v2.1.224+, ListAgents/SendMessage)を有効化し、`claude-remote.service` を「2つの半身を持つ1ユニット」に書き換えた**(バックアップ `.bak-20260808-*`)。

**核心の罠**: `remote-control` 配下のセッションは**永久に発見されない**。子を `--print --sdk-url` の SDK セッションとして起動するため `~/.claude/sessions/<pid>.json` の `entrypoint` が `sdk-cli` になり `bridgeSessionId` を**登録しない** → そのセッションの `ListAgents` は `No reachable agents.`、相手からは `from="unknown"` に見え、受信と「届いた from をコピーした返信」だけの片肺になる。`remote-control --help` に変更オプションは無い。`kind` は両方 `interactive` なので**非対話性は無関係**、切り分けは登録 JSON の `entrypoint` を見るのが最速。PTY を与えると `messagingSocketPath` だけは付くが `bridgeSessionId` は付かない(=リモートからは依然不可視)。

**2つの半身は要求が正反対**で、同じ tmux server に同居できない: peer は**PTY 必須**(PTY のある `entrypoint:"cli"` だけが `bridgeSessionId` を得る)、remote-control は**PTY 禁止**(PTY があると TUI が再描画し 100 ticks/30s ≒3.3% を食い続ける。`TERM=dumb` でも同じ=**TERM ではなく PTY の有無が引き金**)。よって1ユニット内で: `ExecStartPre=/usr/bin/tmux -L claude-peer new-session -d -s peer -x 200 -y 50 /usr/bin/env TERM=xterm-256color %h/.local/bin/claude` + `ExecStart=%h/.local/bin/claude remote-control --name "ThinkPad Always-On"`(MainPID=こちら, `Restart=always` はこちらのみ有効=**peer が死んでも systemd は気づかない**) + `ExecStop=-/usr/bin/tmux -L claude-peer kill-server`。`~/.claude/settings.json` トップレベル `"crossSessionInbound": "accept"`(accept/hold/refuse)が無いと無人セッションは承認できず保留で溜まる(最大100件/既定5分)。accept は**自動実行の入力経路が1本増える**点に注意。関連キー `isolatePeerMachines`。**ListAgents の表示名は登録名(`shota-20` 等)ではなく会話タイトル**なので宛先は一覧の文字列をそのまま使い、同名2行なら ` [ref]` を付ける。テストで立てたセッションはブリッジ側に残骸として残る(ローカル kill 後も一覧に出る)。

**自動更新**(2026-08-08): `autoUpdatesChannel:"latest"` は**設定済みでも効かなかった** — 更新チェックはセッション起動時で、常駐サービスは再起動しないため 2.1.220 で固着していた。バイナリを差し替えても**走っているプロセスは古いまま**(要 restart)。対策 = `claude-update.timer`(daily 04:00 + `RandomizedDelaySec=1h` + `Persistent=true`) → `claude-update.service`(oneshot) → `~/.local/bin/claude-selfupdate.sh` が `readlink -f ~/.local/bin/claude` を前後で比較し、**変わった時だけ** `systemctl --user restart claude-remote.service`(restart は生きている Remote Control セッションを切るため)。なお 2.1.226 の remote-control は PTY 無しでも定常 ~2.3%(138 ticks/60s)食う。peer を止めても 142 ticks/60s で変わらないので **peer も統合も無関係**、v2.1.205 時代の「0.0%」から変化した模様(バージョンを戻しての確認は未実施)。未検証: マシン再起動をまたいだ復活。

**ヘッドレス OAuth 手順**(物理画面なし・リモートから): tmux 内で `claude auth login --claudeai`(または初回 `claude` のオンボーディング)を起動 → `tmux capture-pane -p | grep -E 'oauth/authorize|create_api_key|state=' | tr -d ' \n'` で折り返しURLを再構成してユーザに渡す → ブラウザ認証で出た `code#state` を `tmux send-keys` で貼り戻す。redirect は `platform.claude.com/oauth/code/callback` の out-of-band 方式なので loopback 不要。SSH は `ssh -i ~/.ssh/helloidea_shota_ed25519 shota@192.168.10.100`([[feedback_the_debug_env_is_real]])。
